// EditorController+Markdown.mm — EditorController: the Markdown preview: switching to it, editing from it, links, math and web pictures arriving.
#import "EditorController+Private.h"
#import "EditorController+MarkdownIvars.h"
#import "TextKit2.h"
#import "MarkdownMermaid.h"
#import "MarkdownSidePreview.h"
#include "MarkdownTasks.h"
#include "MermaidDiagram.h"
#import <objc/runtime.h>

@implementation EditorController (Markdown)

- (BOOL)canTogglePreview {
    return self.isMarkdown || self.isLatex || MCViewerKindOverText(self.viewerKind) ||
           MCViewerKindBesideText(self.viewerKind) || MCViewerKindRendersMarkdown(self.viewerKind);
}

// The rendered Markdown preview is up: a Markdown file's, or a notebook's,
// which is Markdown made from it (read-only: double-click editing, task
// boxes and the like stay with Markdown files).
- (BOOL)showsRenderedMarkdown {
    return (self.isMarkdown || MCViewerKindRendersMarkdown(self.viewerKind)) && self.previewMode;
}

// The Markdown that preview renders.
- (NSString *)markdownToRender {
    if (MCViewerKindRendersMarkdown(self.viewerKind)) return MCNotebookMarkdown(self.sourceText ?: @"");
    return self.sourceText;
}

- (void)togglePreview:(id)sender {
    if (!self.canTogglePreview) {
        NSBeep();
        return;
    }
    if (MCViewerKindBesideText(self.viewerKind)) {   // the JSON outline: shown or not
        self.viewers.besideTextShown = !self.viewers.besideTextShown;
        [self relayoutRightArea];
        return;
    }
    [self revealEditor];
    NSTextView *tv = self.textView;
    const BOOL toPreview = !self.previewMode;
    const BOOL overText = MCViewerKindOverText(self.viewerKind);   // a CSV table
    if (overText) [_viewers commitEditing];   // a cell being typed into is kept
    NSInteger sourceLine = -1;       // the caret's line, for the preview to show
    NSInteger previewLine = -1;      // the line at the top of a scrolled preview
    if (toPreview) {
        _sourceSelection = tv.selectedRange;
        NSRect caret = [self textRectForCharacter:_sourceSelection.location];
        _sourceCaretOffset = NSMinY(caret) - NSMinY(tv.visibleRect);
        _haveSourcePosition = !NSIsEmptyRect(caret);
        sourceLine = [self lineOfCharacter:_sourceSelection.location inString:tv.string];
    } else if (self.isMarkdown &&
               !NSEqualPoints(self.editorScroll.contentView.bounds.origin, _previewEntryScroll)) {
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMidX(tv.visibleRect), NSMinY(tv.visibleRect) + 4)];
        previewLine = [self markdownSourceLineNear:top];
    }
    if (tv.editable) self.sourceText = tv.string;  // keep edits
    if (overText && toPreview) [self endTextSessionOfTab:_activeTab];
    self.previewMode = toPreview;
    [self refreshDisplay];
    // The source on show goes to its language server, as when a file opens
    // in the source (marksman, texlab); the rendered page never does.
    [self.lsp documentOpened:self.previewMode ? nil : [self languageServerPath:self.currentPath]];
    [self updateTitle];

    if (toPreview && self.isMarkdown) {
        // Show the part of the page the caret was in, a third of the way down.
        NSUInteger target = [self markdownCharacterForSourceLine:sourceLine];
        NSRect r = [self textRectForCharacter:target];
        if (!NSIsEmptyRect(r))
            [self scrollEditorToY:NSMinY(r) - NSHeight(tv.visibleRect) / 3];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (!toPreview && previewLine >= 0) {
        // The preview was scrolled: open the source where it was looking.
        NSUInteger at = [self characterAtStartOfLine:previewLine inString:tv.string];
        tv.selectedRange = NSMakeRange(at, 0);
        NSRect r = [self textRectForCharacter:at];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - 8];
    } else if (!toPreview && _haveSourcePosition) {
        const NSUInteger n = tv.string.length;
        NSRange sel = _sourceSelection;
        if (sel.location > n) sel = NSMakeRange(n, 0);
        if (NSMaxRange(sel) > n) sel.length = n - sel.location;
        tv.selectedRange = sel;
        NSRect r = [self textRectForCharacter:sel.location];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - _sourceCaretOffset];
    }
    if (!self.previewMode) [self.window makeFirstResponder:tv];
    else if (MCViewerKindOverText(self.viewerKind) && _viewers.focusView)
        [self.window makeFirstResponder:_viewers.focusView];
}

// Back from a CSV's text to its table. The table empties the text view,
// so NSTextView's typing steps on the tab's undo stack would undo into
// nothing (or throw) there: they go, and what they did becomes one step
// from the text as it was put on show, which the table can undo.
- (void)endTextSessionOfTab:(MCTab *)t {
    NSUndoManager *undo = t.undoManager;
    [undo removeAllActionsWithTarget:self.textView.textStorage];
    NSString *start = t.textSessionStart;
    t.textSessionStart = nil;
    NSString *now = t.sourceText ?: @"";
    if (start && ![start isEqualToString:now])
        [self registerViewerUndoFrom:start to:now actionName:@"Typing"];
}

// Where character `i` of the editor is drawn, in the text view's coordinates;
// the end of the text counts as the last character. Empty when it cannot be
// known. The editor runs TextKit 1 (see CLAUDE.md), so this asks the layout
// manager, and it never touches one under TextKit 2, where asking would
// switch the view over.
- (NSRect)textRectForCharacter:(NSUInteger)i {
    return MCTextViewRectForCharacter(self.textView, i);
}

- (void)scrollEditorToY:(CGFloat)y {
    MCScrollTextViewToY(self.editorScroll, self.textView, y);
}

- (NSInteger)lineOfCharacter:(NSUInteger)i inString:(NSString *)s {
    NSInteger line = 0;
    const NSUInteger end = MIN(i, s.length);
    for (NSUInteger k = 0; k < end; k++)
        if ([s characterAtIndex:k] == '\n') line++;
    return line;
}

- (NSUInteger)characterAtStartOfLine:(NSInteger)line inString:(NSString *)s {
    NSUInteger k = 0;
    for (NSInteger l = 0; l < line && k < s.length; k++)
        if ([s characterAtIndex:k] == '\n') l++;
    return k;
}

// The first rendered character that came from source line `line` or later
// (the end of the page when none did).
- (NSUInteger)markdownCharacterForSourceLine:(NSInteger)line {
    NSTextStorage *st = self.textView.textStorage;
    __block NSUInteger found = st.length;
    if (line <= 0) return 0;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        if (v && v.integerValue >= line) { found = r.location; *stop = YES; }
    }];
    return found;
}

// The source line behind rendered character `i`, or the nearest one after it.
- (NSInteger)markdownSourceLineNear:(NSUInteger)i {
    NSTextStorage *st = self.textView.textStorage;
    if (i >= st.length) i = st.length ? st.length - 1 : 0;
    __block NSInteger line = -1;
    if (st.length == 0) return line;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(i, st.length - i) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        (void)r;
        if (v) { line = v.integerValue; *stop = YES; }
    }];
    return line;
}

// ---------------------------------------------- editing from the preview

// An edit from the preview that cannot go in, because the file changed
// while it was being typed (a reload from disk, an agent's edit, Cmd+Z):
// the text goes to the clipboard and the status bar says so, rather than
// the edit vanishing.
static void MCKeepLostMarkdownEdit(EditorController *me, NSString *text) {
    if (!text.length) return;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:text forType:NSPasteboardTypeString];
    [me showStatusNote:@"The file changed while you were editing, so your edit was not "
                       @"applied; your text is on the clipboard."];
}

// A double-click on rendered Markdown opens the block behind it (a
// paragraph, heading, list item, quote, code block or table cell) in a
// popover holding its Markdown; saving splices just those bytes back into
// the buffer, which is then dirty until Cmd+S, as typing leaves it.
- (BOOL)editMarkdownAtCharacter:(NSUInteger)index point:(NSPoint)point {
    if (!self.isMarkdown || !self.previewMode) return NO;
    return [self editMarkdownIn:self.textView atCharacter:index point:point];
}

// The same from the preview to the side: the block's new Markdown goes
// into the source beside it as one replacement and one undo step, as
// typing it there would (applySidePreviewEdit:).
- (BOOL)editSidePreviewAtCharacter:(NSUInteger)index point:(NSPoint)point {
    if (![self sidePreviewShowing] || !self.textView.editable) return NO;
    return [self editMarkdownIn:_sidePreview.textView atCharacter:index point:point];
}

// The block editor over `page`: the full preview's text view (the
// editor's) or the side preview's.
- (BOOL)editMarkdownIn:(NSTextView *)page atCharacter:(NSUInteger)index point:(NSPoint)point {
    const BOOL side = page != self.textView;
    NSTextStorage *st = page.textStorage;
    if (st.length == 0) return NO;
    // The insertion point can fall just after the character clicked.
    if (index >= st.length) index = st.length - 1;
    if (index > 0 && !NSPointInRect(point, MCTextViewRectForCharacter(page, index)) &&
        NSPointInRect(point, MCTextViewRectForCharacter(page, index - 1)))
        index--;
    // A box was ticked by the first click; the second does nothing more.
    id link = [st attribute:NSLinkAttributeName atIndex:index effectiveRange:NULL];
    if ([link isKindOfClass:NSString.class] && [link hasPrefix:kTaskLinkPrefix]) return YES;
    NSNumber *line = [st attribute:kMarkdownSourceLine atIndex:index effectiveRange:NULL];
    if (!line) return NO;
    NSArray<NSNumber *> *cell = [st attribute:kMarkdownTableCell atIndex:index
                               effectiveRange:NULL];
    int sourceLine = line.intValue, column = -1;
    if (cell.count == 2) {
        const int row = cell[0].intValue;
        sourceLine += row + (row > 0 ? 1 : 0);   // the separator row follows the header
        column = cell[1].intValue;
    }
    const std::string src = Utf8String(self.sourceText ?: @"");
    MarkdownEdit::Block block = MarkdownEdit::blockAt(src, sourceLine, column);
    if (block.kind == MarkdownEdit::Block::None) return NO;

    static NSString *const titles[] = {@"", @"Paragraph", @"Heading", @"List item",
                                       @"Quote", @"Code block", @"Table cell", @"Table row",
                                       @"Formula", @"HTML"};
    NSString *title = [titles[block.kind] stringByAppendingString:
        @" (Markdown; Return saves, Shift+Return for a new line)"];
    NSString *text = [NSString stringWithUTF8String:
        src.substr(block.start, block.end - block.start).c_str()] ?: @"";

    NSRect anchor = MCTextViewRectForCharacter(page, index);
    if (NSIsEmptyRect(anchor)) anchor = NSMakeRect(point.x, point.y, 1, 1);
    if (!self.markdownEditPanel) self.markdownEditPanel = [MCMarkdownEditPanel new];
    __weak EditorController *weakSelf = self;
    NSString *before = self.sourceText;
    void (^addItem)(NSString *) = nil;
    if (block.kind == MarkdownEdit::Block::ListItem) {
        addItem = ^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me) return;
            if (![me.sourceText isEqualToString:before]) {
                MCKeepLostMarkdownEdit(me, newText);
                return;
            }
            std::string edited = MarkdownEdit::addItem(src, block, Utf8String(newText));
            [me applyMarkdownEdit:[NSString stringWithUTF8String:edited.c_str()] side:side];
        };
    }
    [self.markdownEditPanel showText:text title:title inView:page rect:anchor
        commit:^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me) return;
            if (![me.sourceText isEqualToString:before]) {   // the file changed meanwhile
                if (![newText isEqualToString:text]) MCKeepLostMarkdownEdit(me, newText);
                return;
            }
            std::string edited = MarkdownEdit::replace(src, block, Utf8String(newText));
            if (edited != src)
                [me applyMarkdownEdit:[NSString stringWithUTF8String:edited.c_str()] side:side];
        }
        addItem:addItem];
    return YES;
}

- (void)applyMarkdownEdit:(NSString *)source side:(BOOL)side {
    if (side) [self applySidePreviewEdit:source];
    else [self applyMarkdownSource:source];
}

// A block edited from the preview to the side: the one stretch that
// differs goes into the source on show through the editor's own
// replacement (CodeTextView replaceRanges:), so it is one undo step, the
// highlighter, folds and server follow it, and the caret stays put; the
// page is rendered again in its place.
- (void)applySidePreviewEdit:(NSString *)source {
    CodeTextView *tv = self.textView;
    NSString *old = tv.string;
    if (!source || [source isEqualToString:old]) return;
    if (!tv.editable || ![self sidePreviewShowing]) {   // the full preview took the slot meanwhile
        [self applyMarkdownSource:source];
        return;
    }
    const NSUInteger a = old.length, b = source.length;
    NSUInteger p = 0, s = 0;
    while (p < a && p < b && [old characterAtIndex:p] == [source characterAtIndex:p]) p++;
    while (s < a - p && s < b - p &&
           [old characterAtIndex:a - 1 - s] == [source characterAtIndex:b - 1 - s])
        s++;
    // Whole UTF-16 pairs only.
    if (p > 0 && CFStringIsSurrogateHighCharacter([old characterAtIndex:p - 1])) p--;
    if (s > 0 && CFStringIsSurrogateLowCharacter([old characterAtIndex:a - s])) s--;
    const NSRange range = NSMakeRange(p, a - p - s);
    NSString *with = [source substringWithRange:NSMakeRange(p, b - p - s)];
    if (![tv replaceRanges:@[[NSValue valueWithRange:range]] withStrings:@[with]
                actionName:@"Preview Edit"])
        return;
    [self renderSidePreviewKeepingPlace];
}

// A new buffer from a preview edit: dirty, undoable with Cmd+Z, and shown
// again without moving the page.
- (void)applyMarkdownSource:(NSString *)source {
    if (!source || [source isEqualToString:self.sourceText]) return;
    NSString *old = self.sourceText ?: @"";
    NSUndoManager *undo = self.textView.undoManager;
    __weak EditorController *weakSelf = self;
    [undo registerUndoWithTarget:self handler:^(EditorController *target) {
        (void)target;
        [weakSelf applyMarkdownSource:old];
    }];
    [undo setActionName:@"Preview Edit"];
    self.sourceText = source;
    [self noteEdited];
    [self updateTitle];
    if (self.isMarkdown && self.previewMode) {
        NSPoint origin = self.editorScroll.contentView.bounds.origin;
        [self renderMarkdown:source];
        [self scrollEditorToY:origin.y];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (self.isMarkdown) {
        [self refreshDisplay];
    }
}

// A click on a link in rendered Markdown. "#section" scrolls to that heading;
// a web address opens in the browser panel (another scheme, such as mailto,
// goes to the system); anything else is a path relative to the Markdown
// file, opened as the terminal opens paths, with "#section" or GitHub's
// "#L12" honoured when the target is a Markdown or source file.
- (void)openMarkdownLink:(NSString *)href {
    if ([href hasPrefix:@"#"]) {
        if (![self scrollToMarkdownAnchor:[href substringFromIndex:1]]) NSBeep();
        return;
    }
    NSURL *url = [NSURL URLWithString:href];
    NSString *scheme = url.scheme.lowercaseString;
    if (scheme.length && ![scheme isEqualToString:@"file"]) {
        if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) {
            MCTermLink *link = [MCTermLink new];
            link.isURL = YES;
            link.target = href;
            [self openTerminalLink:link];
        } else if (url) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
        return;
    }
    NSString *path = href, *fragment = nil;
    if ([scheme isEqualToString:@"file"]) path = url.path;
    NSRange hash = [path rangeOfString:@"#"];
    if (hash.location != NSNotFound) {
        fragment = [path substringFromIndex:hash.location + 1];
        path = [path substringToIndex:hash.location];
    }
    path = (path.stringByRemovingPercentEncoding ?: path).stringByExpandingTildeInPath;
    if (!path.absolutePath)
        path = [self.currentPath.stringByDeletingLastPathComponent
                   stringByAppendingPathComponent:path];
    path = path.stringByStandardizingPath;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir]) {
        NSBeep();
        return;
    }
    MCTermLink *link = [MCTermLink new];
    link.target = path;
    link.isDirectory = dir;
    if (fragment.length > 1 && [fragment characterAtIndex:0] == 'L')
        link.line = [fragment substringFromIndex:1].intValue;   // GitHub's #L12
    [self openTerminalLink:link];
    if (fragment.length && link.line <= 0 && self.isMarkdown && self.previewMode &&
        [self.currentPath isEqualToString:path])
        [self scrollToMarkdownAnchor:fragment];
}

- (BOOL)scrollToMarkdownAnchor:(NSString *)anchor {
    NSString *key = (anchor.stringByRemovingPercentEncoding ?: anchor).lowercaseString;
    NSNumber *at = _markdownAnchors[key];
    if (!at) return NO;
    NSRect r = [self textRectForCharacter:at.unsignedIntegerValue];
    if (NSIsEmptyRect(r)) {
        [self.textView scrollRangeToVisible:NSMakeRange(at.unsignedIntegerValue, 0)];
        return YES;
    }
    [self scrollEditorToY:NSMinY(r) - 8];
    return YES;
}

// The window's Mermaid diagrams (MarkdownMermaid.h), made on first use.
static char kMarkdownMermaidKey;
- (MCMarkdownMermaid *)markdownMermaid {
    MCMarkdownMermaid *m = objc_getAssociatedObject(self, &kMarkdownMermaidKey);
    if (!m) {
        m = [MCMarkdownMermaid new];
        __weak EditorController *weakSelf = self;
        m.onArrived = ^{
            EditorController *s = weakSelf;
            [s renderShownMarkdownKeepingPlace];
        };
        objc_setAssociatedObject(self, &kMarkdownMermaidKey, m, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return m;
}

// The full preview: the page in the editor's own text view, at its top.
- (void)renderMarkdown:(NSString *)content {
    NSTextContainer *box = self.textView.textContainer;
    NSDictionary *anchors = nil;
    NSAttributedString *page =
        [self markdownPageFor:content width:box.size.width - 2 * box.lineFragmentPadding
                      anchors:&anchors];
    _markdownAnchors = anchors;
    _liveHighlight = NO;   // rendered Markdown is not source
    [self layoutHeldTextBlocks];   // its quotes and code boxes (displaySourceEditable)
    [self.textView.textStorage setAttributedString:page];
    [self.textView scrollToBeginningOfDocument:nil];
    [self finishMarkdownPage];
}

// Formulas the page just shown has not typeset yet go to tectonic in one
// run, and its diagrams to mmdc; the page renders again as they come back
// (markdownMathArrived:, markdownMermaid's onArrived).
- (void)finishMarkdownPage {
    if (_mathWanted.count) MCMathTypeset(_mathWanted);
    _mathWanted = nil;
    [[self markdownMermaid] endPage];
}

// Rendered Markdown for `content`, laid out for a text `width` points wide,
// with its headings' and footnotes' places by anchor. What it waits for
// (math, web pictures, diagrams) is noted for the page that shows it;
// finishMarkdownPage then sends it off. Shared by the full preview and the
// one beside the source, which never show at once.
- (NSMutableAttributedString *)markdownPageFor:(NSString *)content
                                         width:(CGFloat)width
                                       anchors:(NSDictionary **)anchorsOut {
    _markdownPageWidth = width;
    std::string md = content.UTF8String ? content.UTF8String : "";
    std::vector<MdRun> runs = MarkdownParser::parse(md);
    MCMarkdownMermaid *mermaid = [self markdownMermaid];   // ```mermaid blocks
    [mermaid beginPageDark:[AppSettings shared].settings.isDark()];
    NSString *mermaidComplaint = nil, *mermaidKey = nil;  // of the block being shown as code
    size_t mermaidEnd = 0;
    _awaitedWebImages = [NSMutableSet set];   // markdownRun: fills it
    _webImageMarks = 0;
    _awaitedMath = [NSMutableSet set];        // and these
    _mathWanted = [NSMutableArray array];
    _mathMarks = 0;
    _mathOn = [self typesetsMath];            // asked once, not per formula
    _mathRunProblem = nil;                    // markdownRun: notes a failed run

    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSMutableDictionary<NSString *, NSNumber *> *anchors = [NSMutableDictionary dictionary];
    // A paragraph's look comes from its first run, and every run in it gets
    // that paragraph style; a quote's or a code block's paragraphs share one
    // text block, which draws the bar or the box around all of them.
    NSParagraphStyle *para = nil;
    NSMutableArray<NSTextBlock *> *quoteBlocks = [NSMutableArray array];   // one a level
    NSTextBlock *codeBox = nil;
    BOOL lineStart = YES;
    MCMarkdownCodeBlocks *codeColors = [MCMarkdownCodeBlocks new];   // ```python and the like
    for (size_t i = 0; i < runs.size();) {
        const MdRun &r = runs[i];
        if (r.tableId > 0) {
            size_t end = i;
            while (end < runs.size() && runs[end].tableId == r.tableId) end++;
            [self appendMarkdownTable:runs from:i to:end into:out];
            i = end;
            lineStart = YES;
            [quoteBlocks removeAllObjects];
            codeBox = nil;
            continue;
        }
        if (lineStart && i >= mermaidEnd) {
            // A diagram mmdc has drawn takes its block's place; until then,
            // and when it cannot be drawn, the block shows as code.
            size_t end = i;
            NSAttributedString *pic = [mermaid pictureForRuns:runs at:i end:&end
                                                      lineKey:kMarkdownSourceLine
                                                    complaint:&mermaidComplaint key:&mermaidKey];
            if (pic) {
                [out appendAttributedString:pic];
                i = end;
                [quoteBlocks removeAllObjects];
            codeBox = nil;
                continue;
            }
            if (mermaidComplaint || mermaidKey) {
                mermaidEnd = i;
                while (mermaidEnd < runs.size() && MermaidDiagram::isMermaidRun(runs[mermaidEnd]))
                    mermaidEnd++;
            }
        }
        if (lineStart) {
            if (r.heading > 0) {
                // A heading's text may be several runs (bold, code, links).
                std::string title;
                for (size_t k = i; k < runs.size() && runs[k].heading > 0 &&
                                   runs[k].line == r.line; k++)
                    title += runs[k].text;
                // The core's anchor, so links land where they do on Linux and Android.
                const std::string anchor = MarkdownParser::anchor(title);
                NSString *slug = [[NSString alloc] initWithBytes:anchor.data() length:anchor.size()
                                                        encoding:NSUTF8StringEncoding] ?: @"";
                // GitHub numbers repeats: intro, intro-1, intro-2.
                NSString *key = slug;
                for (int n = 1; anchors[key]; n++)
                    key = [NSString stringWithFormat:@"%@-%d", slug, n];
                if (key.length) anchors[key] = @(out.length);
            }
            para = [self markdownParagraphFor:r quotes:quoteBlocks code:&codeBox];
        }
        if (!r.anchor.empty()) {   // a footnote, or a reference to one, that links land on
            NSString *key = [NSString stringWithUTF8String:r.anchor.c_str()];
            if (key.length && !anchors[key]) anchors[key] = @(out.length);
        }
        NSAttributedString *piece = [self markdownRun:r style:para inCell:NO];
        if (piece.length && i < mermaidEnd && (mermaidComplaint || mermaidKey)) {
            NSMutableAttributedString *m = [piece mutableCopy];
            const NSRange all = NSMakeRange(0, m.length);
            if (mermaidComplaint) [m addAttribute:NSToolTipAttributeName value:mermaidComplaint range:all];
            if (mermaidKey) [m addAttribute:MCMarkdownMermaidAttribute value:mermaidKey range:all];
            piece = m;
        }
        if (piece.length) {
            if (r.codeBlock) [codeColors noteRun:NSMakeRange(out.length, piece.length) language:r.lang];
            [out appendAttributedString:piece];
            lineStart = [piece.string hasSuffix:@"\n"];
        }
        i++;
    }
    [codeColors colorPage:out];
    // A page whose math cannot be typeset says why in a line at its top.
    BOOL hasMath = NO;
    for (const MdRun &r : runs) hasMath = hasMath || r.math;
    NSAttributedString *note = !hasMath ? nil
                             : self.mathNoteWanted ? [self mathNote]
                             : _mathRunProblem ? [self mathNoteForProblem:_mathRunProblem]
                             // Formulas still out with tectonic: say so, or the TeX
                             // standing in looks like math that will never be typeset.
                             : _awaitedMath.count ? [self mathNoteTypesetting:_awaitedMath.count]
                             : nil;
    if (note) {
        [out insertAttributedString:note atIndex:0];
        for (NSString *key in anchors.allKeys)
            anchors[key] = @(anchors[key].unsignedIntegerValue + note.length);
    }
    if (anchorsOut) *anchorsOut = anchors;
    return out;
}

// ------------------------------------------------------ math in the preview

// Whether formulas are typeset: the setting, and tectonic to do it.
- (BOOL)typesetsMath {
    return [AppSettings shared].settings.math() && MCTectonicPath() != nil;
}

// The line at the top of a page with math is wanted when typesetting is
// on but tectonic is not there.
- (BOOL)mathNoteWanted {
    return [AppSettings shared].settings.math() && MCTectonicPath() == nil;
}

// The line at the top of a page, muted, with a link at its end if any.
- (NSAttributedString *)mathNoteText:(NSString *)text link:(NSString *)link {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacing = 10;
    NSDictionary *muted = @{
        NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(12)],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
        NSParagraphStyleAttributeName: ps,
        kMarkdownMathNote: @YES,
    };
    NSMutableAttributedString *note = [[NSMutableAttributedString alloc] init];
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:muted]];
    if (link) {
        NSMutableDictionary *a = [muted mutableCopy];
        a[NSLinkAttributeName] = kMathDownloadLink;
        a[NSForegroundColorAttributeName] = [[AppSettings shared] markdown:MarkdownColor::Link];
        [note appendAttributedString:[[NSAttributedString alloc] initWithString:link attributes:a]];
    }
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:muted]];
    return note;
}

// A whole run failed (tectonic could not fetch its files, say): why, and
// that it is tried again.
- (NSAttributedString *)mathNoteForProblem:(NSString *)problem {
    NSString *why = [problem stringByTrimmingCharactersInSet:
                                 NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([why hasSuffix:@"."]) why = [why substringToIndex:why.length - 1];
    return [self mathNoteText:[NSString stringWithFormat:
        @"The math on this page shows as TeX: tectonic could not typeset it (%@). "
        @"It is tried again when the page is next shown, a minute from now or later.", why]
                         link:nil];
}

// tectonic keeps the LaTeX files it downloads in its own cache. Before its
// first run there is none, and that run fetches about 42 MB first.
static BOOL MCTectonicCacheIsEmpty(void) {
    NSString *dir = NSProcessInfo.processInfo.environment[@"TECTONIC_CACHE_DIR"];
    if (!dir.length)
        dir = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)
                   .firstObject stringByAppendingPathComponent:@"TectonicProject.Tectonic"];
    NSArray *inside = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
    return inside.count == 0;
}

// "Typesetting 12 formulas…" while they are out with tectonic, with a word
// on the wait when this is tectonic's first run.
- (NSAttributedString *)mathNoteTypesetting:(NSUInteger)count {
    NSString *text = count == 1 ? @"Typesetting 1 formula…"
                                : [NSString stringWithFormat:@"Typesetting %lu formulas…",
                                                             (unsigned long)count];
    if (MCTectonicCacheIsEmpty())
        text = [text stringByAppendingString:
            @" tectonic's first run downloads its LaTeX files first, which takes about a minute."];
    return [self mathNoteText:text link:nil];
}

// "Math shows as TeX..." with a link that downloads tectonic, or how that
// download is going.
- (NSAttributedString *)mathNote {
    NSString *text, *link = nil;
    if (MCTectonicDownloading()) {
        text = @"Downloading tectonic, which typesets the math on this page…";
    } else if (_tectonicProblem) {
        text = [NSString stringWithFormat:@"tectonic could not be downloaded (%@), so the math "
                                          @"shows as TeX. ", _tectonicProblem];
        link = @"Try again";
    } else {
        text = @"The math on this page shows as TeX: MiniCode typesets it with tectonic, "
               @"which is not installed. ";
        link = @"Download tectonic";
    }
    return [self mathNoteText:text link:link];
}

- (void)downloadTectonicForMath {
    if (MCTectonicDownloading()) return;
    _tectonicProblem = nil;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    MCDownloadTectonic(^(NSString *problem) {
        EditorController *me = weakSelf;
        if (!me) return;
        me->_tectonicProblem = [problem copy];
        if (me.isMarkdown && [path isEqualToString:me.currentPath])
            [me renderShownMarkdownKeepingPlace];
    });
    [self renderShownMarkdownKeepingPlace];   // "Downloading..."
}

// How long the line about tectonic at the top of the page is, or 0.
static NSUInteger MCMathNoteLength(NSTextStorage *st) {
    NSRange r = NSMakeRange(0, 0);
    if (st.length && [st attribute:kMarkdownMathNote atIndex:0 longestEffectiveRange:&r
                                inRange:NSMakeRange(0, st.length)])
        return r.length;
    return 0;
}

- (NSUInteger)mathNoteLength {
    return MCMathNoteLength(self.textView.textStorage);
}

// The typeset formula for a math run, or nil when it is to show as TeX:
// typesetting is off, it is still being typeset, or it failed.
- (MCMathFormula *)typesetFormulaFor:(const MdRun &)r {
    if (!r.math || !_mathOn) return nil;
    NSString *tex = [NSString stringWithUTF8String:r.text.c_str()];
    if (!tex) return nil;
    MCMathFormula *f = MCMathLookup(tex, r.math == 2);
    return f && !f.failed ? f : nil;
}


// Some formulas of the page on show are back from tectonic: render it again
// with them in, once for all that arrive together, keeping the place.
- (void)markdownMathArrived:(NSNotification *)note {
    NSSet<NSString *> *keys = note.userInfo[MCMathKeysKey];
    if (!_awaitedMath || ![_awaitedMath intersectsSet:keys]) return;
    [_awaitedMath minusSet:keys];
    if (_mathRenderQueued) return;
    _mathRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedMathForPath:path];
    });
}

- (void)showArrivedMathForPath:(NSString *)path {
    _mathRenderQueued = NO;
    if (!_awaitedMath || ![self showsMarkdownPage] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderShownMarkdownKeepingPlace];
}

// A web picture finished. When the page on show waits for it, the page is
// rendered again with the picture in, and pictures finishing close together
// share one render. A failure keeps the alt text and needs nothing.
- (void)markdownWebImageArrived:(NSNotification *)note {
    NSString *src = note.userInfo[MCMarkdownWebImageURLKey];
    if (!src || ![_awaitedWebImages containsObject:src]) return;
    [_awaitedWebImages removeObject:src];
    if (![note.userInfo[MCMarkdownWebImageOKKey] boolValue] || _webImageRenderQueued) return;
    _webImageRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedWebImagesForPath:path];
    });
}

- (void)showArrivedWebImagesForPath:(NSString *)path {
    _webImageRenderQueued = NO;
    // Not after the window closed, another file was opened, or the source
    // took the preview's place (the preview renders anew when it is back),
    // unless the page beside the source waits for it.
    if (!_awaitedWebImages || ![self showsMarkdownPage] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderShownMarkdownKeepingPlace];
}

// Render the preview again, keeping the text at the top of the pane where it
// was (keepPlaceIn:scroll:whileRendering:). A page at the very top stays at
// the top, as a browser's does while its pictures load.
- (void)renderMarkdownKeepingPlace {
    NSClipView *clip = self.editorScroll.contentView;
    const BOOL unscrolled = NSEqualPoints(clip.bounds.origin, _previewEntryScroll);
    __weak EditorController *weakSelf = self;
    [self keepPlaceIn:self.textView scroll:self.editorScroll whileRendering:^{
        EditorController *me = weakSelf;
        [me renderMarkdown:[me markdownToRender]];
    }];
    if (unscrolled) _previewEntryScroll = clip.bounds.origin;
}

// `render` replaces the page in `tv` (scrolled by `scroll`): the text at the
// top of the pane is put back where it was. Each web picture that has
// arrived takes the place of its alt text, and each typeset formula or
// drawn diagram the place of its source, one character for many, and moves
// what follows by the height it adds, so the text to keep is found again by
// its character, counted past those replacements (and past the line about
// tectonic, when it comes or goes). A page at its top is left at its top.
- (void)keepPlaceIn:(NSTextView *)tv scroll:(NSScrollView *)scroll
     whileRendering:(void (^)(void))render {
    NSTextStorage *st = tv.textStorage;
    NSClipView *clip = scroll.contentView;
    const NSPoint origin = clip.bounds.origin;
    NSUInteger anchor = NSNotFound;
    CGFloat offset = 0;
    if (origin.y > 0 && st.length > 0) {
        const NSRect visible = tv.visibleRect;
        NSUInteger i = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        if (i >= st.length) i = st.length - 1;
        const NSRect r = MCTextViewRectForCharacter(tv, i);
        if (!NSIsEmptyRect(r)) {
            offset = NSMinY(r) - NSMinY(visible);
            // Stretches before the anchor that change length: where they
            // are, and how many characters they will be.
            NSMutableArray<NSArray<NSNumber *> *> *changes = [NSMutableArray array];
            const NSRange before = NSMakeRange(0, i + 1);
            [st enumerateAttribute:kMarkdownWebImage inRange:before options:0
                        usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                (void)stop;
                if (mark && MCMarkdownWebImageIsReady(mark.firstObject))
                    [changes addObject:@[@(range.location), @(range.length), @1]];
            }];
            [st enumerateAttribute:MCMarkdownMermaidAttribute inRange:before options:0
                        usingBlock:^(NSString *key, NSRange range, BOOL *stop) {
                (void)stop;
                // A diagram's code turns into one picture and its line break.
                if (key && MCMermaidIsReady(key))
                    [changes addObject:@[@(range.location), @(range.length), @2]];
            }];
            if ([self typesetsMath])
                [st enumerateAttribute:kMarkdownMath inRange:before options:0
                            usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                    (void)stop;
                    if (mark && MCMathIsReady(mark.firstObject))
                        [changes addObject:@[@(range.location), @(range.length), @1]];
                }];
            [changes sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) {
                return [a[0] compare:b[0]];
            }];
            NSUInteger shift = 0, inside = NSNotFound;
            for (NSArray<NSNumber *> *c in changes) {
                const NSRange range = NSMakeRange(c[0].unsignedIntegerValue,
                                                  c[1].unsignedIntegerValue);
                if (NSLocationInRange(i, range)) {
                    inside = range.location - shift;
                    break;
                }
                shift += range.length - c[2].unsignedIntegerValue;
            }
            anchor = inside != NSNotFound ? inside : i - shift;
        }
    }
    const NSUInteger noteBefore = MCMathNoteLength(st);
    render();
    // The line about tectonic at the top may have come or gone.
    const NSUInteger noteAfter = MCMathNoteLength(st);
    if (anchor != NSNotFound) {
        if (anchor >= noteBefore) anchor = anchor - noteBefore + noteAfter;
        else anchor = 0;
    }
    if (anchor != NSNotFound && st.length > 0) {
        if (!MCUsesTextKit2(tv))   // TextKit 1, as the editor is: the full height
            [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        const NSRect r = MCTextViewRectForCharacter(tv, MIN(anchor, st.length - 1));
        MCScrollTextViewToY(scroll, tv, NSIsEmptyRect(r) ? origin.y : NSMinY(r) - offset);
    }
}

// ------------------------------------------------ the preview to the side
// Control+Command+V (VS Code's Cmd+K V; minicode has no chords and gives
// Cmd+K chords Control+Command and the chord's second key, as the folding
// keys have): the tab's page beside its source, which stays on show and
// keeps the keyboard. The pane is the window's (MarkdownSidePreview.h); a
// tab wants it or not (MCTab sidePreview, kept in the session), and the
// page is rendered by markdownPageFor: as the full preview's is, 0.3 s
// after typing stops, keeping its place. While the tab shows its full
// preview (Shift+Cmd+V) the pane steps aside, and comes back with the
// source.

static const NSTimeInterval kSidePreviewDelay = 0.3;

// The active tab wants the pane and has its source on show.
- (BOOL)sidePreviewWanted {
    MCTab *t = _activeTab;
    return t.sidePreview && t.isMarkdown && !t.previewMode && !t.showingMessage && !t.isDiff &&
           t.viewerKind == MCViewerNone && t.currentPath != nil;
}

// The pane is on show with the active tab's page.
- (BOOL)sidePreviewShowing {
    return _sidePreview.shown && _sidePreviewTab && _sidePreviewTab == _activeTab &&
           [self sidePreviewWanted];
}

- (BOOL)canToggleSidePreview {
    return self.isMarkdown && _activeTab != nil && !self.showingMessage && !self.isDiff &&
           self.currentPath != nil;
}

// A rendered page is on show: the full preview, or the one beside the source.
- (BOOL)showsMarkdownPage {
    return [self showsRenderedMarkdown] || [self sidePreviewShowing];
}

// Render whichever page is on show again, keeping its place (a formula, a
// picture or a diagram arrived, the zoom changed).
- (void)renderShownMarkdownKeepingPlace {
    if ([self showsRenderedMarkdown]) [self renderMarkdownKeepingPlace];
    else if ([self sidePreviewShowing]) [self renderSidePreviewKeepingPlace];
}

- (MCMarkdownSidePreview *)sidePreview {
    if (_sidePreview) return _sidePreview;
    _sidePreview = [[MCMarkdownSidePreview alloc] initInView:self.rightArea];
    __weak EditorController *weakSelf = self;
    _sidePreview.onRender = ^{
        EditorController *me = weakSelf;
        if ([me sidePreviewShowing]) [me renderSidePreviewKeepingPlace];
    };
    _sidePreview.onLink = ^BOOL(id link, NSUInteger index) {
        (void)index;
        return [weakSelf sidePreviewClickedLink:link];
    };
    _sidePreview.onDrag = ^(CGFloat x) { [weakSelf sidePreviewBarDraggedTo:x]; };
    _sidePreview.onClose = ^{ [weakSelf closeSidePreview]; };
    _sidePreview.onDoubleClick = ^BOOL(NSUInteger index, NSPoint point) {
        return [weakSelf editSidePreviewAtCharacter:index point:point];
    };
    // Scrolling with the source, and marking its caret's block, by the
    // editor's line index (the gutter's, kept whether it shows or not).
    _sidePreview.sourceLineKey = kMarkdownSourceLine;
    _sidePreview.following = ^BOOL { return [weakSelf sidePreviewShowing]; };
    __weak MCLineGutter *gutter = self.gutter;
    [_sidePreview followSource:self.textView
                        scroll:self.editorScroll
               lineOfCharacter:^NSUInteger(NSUInteger i) { return [gutter lineOfCharacter:i]; }
                   startOfLine:^NSUInteger(NSUInteger l) { return [gutter startOfLine:l]; }
                     lineCount:^NSUInteger { return gutter.lineCount; }];
    return _sidePreview;
}

// The divider between the source and the pane, for the right area's bars.
- (NSView *)sidePreviewDivider { return _sidePreview.divider; }

// relayoutRightArea: the source's slot, `rect`, split with the pane when
// the tab wants it and its text is on show (`textShown`). Returns the
// source's part. A page left from a tab no longer on show goes.
- (NSRect)layoutSidePreviewIn:(NSRect)rect textShown:(BOOL)textShown {
    const BOOL wanted = [self sidePreviewWanted];
    if (_sidePreviewTab && (!wanted || _sidePreviewTab != _activeTab)) [self dropSidePreviewPage];
    if (!_sidePreview && !(wanted && textShown)) return rect;
    return [[self sidePreview] layoutInRect:rect shown:wanted && textShown];
}

- (void)dropSidePreviewPage {
    [_sidePreview clear];
    _sidePreviewTab = nil;
}

// The source is on show (refreshDisplay): the page beside it, the tab's
// own where it was scrolled when it left, or this one's again where it is.
- (void)refreshSidePreview {
    if (![self sidePreviewWanted]) {
        if (_sidePreviewTab) [self dropSidePreviewPage];
        return;
    }
    [self sidePreview];
    if (_sidePreviewTab == _activeTab && _sidePreview.textView.textStorage.length) {
        [self renderSidePreviewKeepingPlace];
        return;
    }
    _sidePreviewTab = _activeTab;
    [self renderSidePreview];
    // A page the user had scrolled is put back where it was (the source
    // was following it, and comes back where it was too); otherwise the
    // page follows the source, as VS Code opens a preview at the editor's
    // top line.
    if (_activeTab.sidePreviewY > 0) {
        _sidePreview.scrollY = _activeTab.sidePreviewY;
        _sidePreview.pageLeads = YES;
    }
    [_sidePreview pageRendered];
}

// The active tab leaves the screen (stashActiveTab): where its page was,
// when the user had scrolled it.
- (void)sidePreviewTabWillLeave:(MCTab *)t {
    if (_sidePreviewTab != t) return;
    t.sidePreviewY = _sidePreview.pageLeads ? _sidePreview.scrollY : 0;
    [self dropSidePreviewPage];
}

// The page for the buffer as it is now, at its top.
- (void)renderSidePreview {
    MCMarkdownSidePreview *side = [self sidePreview];
    [side cancelRender];
    NSDictionary *anchors = nil;
    NSAttributedString *page = [self markdownPageFor:self.sourceText ?: @""
                                               width:side.pageWidth anchors:&anchors];
    side.anchors = anchors;
    [side showPage:page];
    [self finishMarkdownPage];
}

- (void)renderSidePreviewKeepingPlace {
    if (!_sidePreview) return;
    __weak EditorController *weakSelf = self;
    MCMarkdownSidePreview *side = _sidePreview;
    // The page keeps its place while it is rendered (the source does not
    // follow those moves), then follows the source unless the user had
    // scrolled the page last.
    [side holdFollowing:^{
        EditorController *me = weakSelf;
        [me keepPlaceIn:side.textView scroll:side.scrollView whileRendering:^{
            [weakSelf renderSidePreview];
        }];
    }];
    [side pageRendered];
}

// The buffer changed (textDidChange:): the page follows once typing pauses.
// Typed in the source, the source leads again; an edit made from the page
// (a task box, the block editor) leaves the page where the user has it.
- (void)sidePreviewSourceChanged {
    if (_sidePreviewTab && _sidePreviewTab == _activeTab && [self sidePreviewWanted]) {
        if (self.window.firstResponder == self.textView) _sidePreview.pageLeads = NO;
        [_sidePreview scheduleRenderAfter:kSidePreviewDelay];
    }
}

- (void)sidePreviewApplySettings {
    if (!_sidePreview) return;
    [_sidePreview applySettings];
    if ([self sidePreviewShowing]) [self renderSidePreviewKeepingPlace];
}

- (void)toggleSidePreview:(id)sender {
    (void)sender;
    if (![self canToggleSidePreview]) {
        NSBeep();
        return;
    }
    MCTab *t = _activeTab;
    if (t.sidePreview && [self sidePreviewWanted]) {
        [self closeSidePreview];
        return;
    }
    t.sidePreview = YES;
    if (self.previewMode) {
        // From the full preview: the source comes back, with the page beside it.
        [self togglePreview:nil];
    } else {
        [self revealEditor];
        [self relayoutRightArea];
        [self refreshSidePreview];
    }
    [self saveSession];
    if (self.hintsVisible) [self updateHints];
}

- (void)closeSidePreview {
    MCTab *t = _activeTab;
    if (!t.sidePreview) return;
    t.sidePreview = NO;
    t.sidePreviewY = 0;
    const BOOL hadKeyboard = _sidePreview && self.window.firstResponder == _sidePreview.textView;
    [self dropSidePreviewPage];
    [self relayoutRightArea];
    if (hadKeyboard) [self.window makeFirstResponder:self.textView];
    [self saveSession];
    if (self.hintsVisible) [self updateHints];
}

// Dragging the bar between the source and the pane; near the right edge
// it closes the pane, as the browser's bar hides the browser.
- (void)sidePreviewBarDraggedTo:(CGFloat)x {
    if (![_sidePreview dragTo:x]) {
        [self closeSidePreview];
        return;
    }
    [self relayoutRightArea];
}

// A link clicked in the pane: a heading or a footnote scrolls the pane, a
// task's box ticks the task in the source (one undo step, as a click in
// the full preview is), and the rest open as from the full preview.
- (BOOL)sidePreviewClickedLink:(id)link {
    if (![link isKindOfClass:NSString.class]) return NO;
    NSString *href = link;
    if ([href hasPrefix:kTaskLinkPrefix]) {
        const int line = [[href substringFromIndex:kTaskLinkPrefix.length] intValue];
        const std::string src = Utf8String(self.sourceText ?: @"");
        const std::string edited = MarkdownTasks::toggleBox(src, line);
        if (edited == src || !self.textView.editable) {
            NSBeep();
            return YES;
        }
        [self replaceSourceWithText:[NSString stringWithUTF8String:edited.c_str()]
                         actionName:@"Toggle Task"];
        [self renderSidePreviewKeepingPlace];
        return YES;
    }
    if ([href isEqualToString:kMathDownloadLink]) {
        [self downloadTectonicForMath];
        return YES;
    }
    if ([href hasPrefix:@"#"]) {
        if (![_sidePreview scrollToAnchor:[href substringFromIndex:1]]) NSBeep();
        return YES;
    }
    [self openMarkdownLink:href];
    return YES;
}

@end
