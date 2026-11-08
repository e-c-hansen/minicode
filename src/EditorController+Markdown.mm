// EditorController+Markdown.mm — EditorController: the Markdown preview: switching to it, editing from it, links, math and web pictures arriving.
#import "EditorController+Private.h"
#import "EditorController+MarkdownIvars.h"
#import "TextKit2.h"
#import "MarkdownMermaid.h"
#import "MarkdownSidePreview.h"
#include "MarkdownHtml.h"
#include "MarkdownPage.h"
#include "MarkdownTasks.h"
#include "FolderSearch.h"
#include "MermaidDiagram.h"
#import <objc/runtime.h>
#include <unordered_map>

// ------------------------------------------- the side page, in pieces
// The page beside the source is kept as pieces (MarkdownPage.h): for each,
// the hashes of the runs it was built from, how long its text is, its
// source line objects, the places links land on, and whether it may be
// kept.

@interface MCSidePiece : NSObject
@property(nonatomic) NSUInteger length;
@property(nonatomic) int base;
@property(nonatomic, strong) NSArray<MCMarkdownLine *> *lines;
@property(nonatomic, strong) NSArray<NSArray *> *anchors;
@property(nonatomic) BOOL keep;
- (void)moveToBase:(int)base;
@end

@implementation MCSidePiece
// The same runs, now starting at line `base`: its lines move with it.
- (void)moveToBase:(int)base {
    const int delta = base - _base;
    if (delta == 0) return;
    for (MCMarkdownLine *l in _lines) l.line += delta;
    _base = base;
}
@end

@interface MCSidePage : NSObject {
  @public
    std::vector<MdRun> runs;   // until its pieces are built
    std::vector<MarkdownPage::Piece> pieces;
    // Its pieces paired with matchedTo's (MarkdownPage::match), when the
    // queue that read it did that too.
    std::vector<long> match;
}
@property(nonatomic, strong) MCSidePage *matchedTo;
@property(nonatomic, copy) NSString *context;   // what its text depends on beside its runs
@property(nonatomic, strong) NSArray<MCSidePiece *> *info;
@property(nonatomic) NSUInteger noteLength;     // the line about math at its top
@property(nonatomic) NSUInteger length;         // all of its text
@end

@implementation MCSidePage
@end

// The runs and pieces of `text`; any thread.
static MCSidePage *MCSidePageParse(NSString *text) {
    MCSidePage *page = [MCSidePage new];
    const char *md = text.UTF8String;
    page->runs = MarkdownParser::parse(md ? md : "");
    page->pieces = MarkdownPage::split(page->runs);
    return page;
}

// This thread's CPU time, in milliseconds (MarkdownSidePreview's
// lastRenderCpuMilliseconds).
static double MCThreadCpuMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static dispatch_queue_t MCSidePageQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("MiniCode side preview", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

// The character at the top of a page's view, and how far below the view's
// top its line starts.
static NSUInteger MCPageTopCharacter(NSTextView *tv, CGFloat *offset) {
    NSTextStorage *st = tv.textStorage;
    const NSRect visible = tv.visibleRect;
    NSUInteger i = [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(visible) + 20,
                                                                    NSMinY(visible) + 1)];
    if (i >= st.length) i = st.length - 1;
    const NSRect r = MCTextViewRectForCharacter(tv, i);
    if (NSIsEmptyRect(r)) return NSNotFound;
    *offset = NSMinY(r) - NSMinY(visible);
    return i;
}

static void MCAddMarkdownAnchors(NSMutableDictionary<NSString *, NSNumber *> *anchors,
                                 NSArray<NSArray *> *found, NSUInteger shift);

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

// Character `i`'s line `offset` points below the top of the pane (a tab
// shown again, the zoom). Laid out a screen past it too, or the scroll stops
// where the layout does, with that character at the bottom of the pane.
// With non-contiguous layout (large-file mode) not from the top: 300,000
// lines of a 25 MB file took 13 s of the main thread to show again. Its
// rects are estimates that laying out around them can move, so the
// character is placed again until it stays put.
- (void)scrollEditorToCharacter:(NSUInteger)i offset:(CGFloat)offset {
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    if (n == 0) return;
    const NSUInteger at = MIN(i, n - 1);
    NSRect r = [self textRectForCharacter:at];
    const BOOL estimated = tv.layoutManager.allowsNonContiguousLayout;
    const CGFloat screen = NSHeight(self.editorScroll.contentView.bounds);
    for (int pass = 0; pass < 4 && !NSIsEmptyRect(r); pass++) {
        const CGFloat from = estimated ? MAX(0, NSMinY(r) - screen) : 0;
        if (!MCUsesTextKit2(tv))
            [tv.layoutManager ensureLayoutForBoundingRect:
                NSMakeRect(0, from, CGFLOAT_MAX, NSMaxY(r) + 2 * screen - from)
                                          inTextContainer:tv.textContainer];
        if (estimated) r = [self textRectForCharacter:at];
        [self scrollEditorToY:NSMinY(r) - offset];
        if (!estimated) break;
        const NSRect now = [self textRectForCharacter:at];
        if (NSMinY(now) == NSMinY(r)) break;
        r = now;
    }
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
    if ([self catchUpSidePage]) return YES;
    return [self editMarkdownIn:_sidePreview.textView atCharacter:index point:point];
}

// Whether the page beside the source was rendered from the buffer as it is
// now. It can be up to a pause and a render behind the typing, and a click
// on it then names old source lines: such a click renders the page now and
// says so, acting on nothing (YES); the next click lands on the page as it
// is.
- (BOOL)sidePageIsCurrent {
    NSString *shown = _sidePreview.renderedSource, *now = self.sourceText ?: @"";
    return shown == now || [shown isEqualToString:now];
}

- (BOOL)catchUpSidePage {
    if ([self sidePageIsCurrent]) return NO;
    [self renderSidePreviewKeepingPlace];
    [self showStatusNote:@"The preview was behind your typing and has caught up: click again"];
    return YES;
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
    const int lineOf = MCMarkdownLineOf([st attribute:kMarkdownSourceLine atIndex:index
                                                   effectiveRange:NULL]);
    if (lineOf < 0) return NO;
    NSArray<NSNumber *> *cell = [st attribute:kMarkdownTableCell atIndex:index
                               effectiveRange:NULL];
    int sourceLine = lineOf, column = -1;
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
// one beside the source, which never show at once; the one beside the
// source builds its page a piece at a time (renderSidePreviewFrom:) with the
// same three steps: beginMarkdownPageWidth:, markdownRuns:from:to:anchors:
// and markdownNoteForRuns:.
- (NSMutableAttributedString *)markdownPageFor:(NSString *)content
                                         width:(CGFloat)width
                                       anchors:(NSDictionary **)anchorsOut {
    std::string md = content.UTF8String ? content.UTF8String : "";
    std::vector<MdRun> runs = MarkdownParser::parse(md);
    [self beginMarkdownPageWidth:width];
    NSMutableArray<NSArray *> *found = [NSMutableArray array];
    NSMutableAttributedString *out = [self markdownRuns:runs from:0 to:runs.size() anchors:found];
    // A page whose math cannot be typeset says why in a line at its top.
    NSAttributedString *note = [self markdownNoteForRuns:runs];
    if (note) [out insertAttributedString:note atIndex:0];
    NSMutableDictionary<NSString *, NSNumber *> *anchors = [NSMutableDictionary dictionary];
    MCAddMarkdownAnchors(anchors, found, note.length);
    if (anchorsOut) *anchorsOut = anchors;
    return out;
}

// A new page: what its runs will note as they are built (formulas and
// pictures to wait for, diagrams to draw) starts empty.
- (void)beginMarkdownPageWidth:(CGFloat)width {
    _markdownPageWidth = width;
    [[self markdownMermaid] beginPageDark:[AppSettings shared].settings.isDark()];   // ```mermaid
    _awaitedWebImages = [NSMutableSet set];   // markdownRun: fills it
    _webImageMarks = 0;
    _awaitedMath = [NSMutableSet set];        // and these
    _mathWanted = [NSMutableArray array];
    _mathMarks = 0;
    _mathOn = [self typesetsMath];            // asked once, not per formula
    _mathRunProblem = nil;                    // markdownRun: notes a failed run
}

// The places links land on, in page order: @[@YES, a heading's anchor,
// offset] (GitHub numbers repeats: intro, intro-1, intro-2) or @[@NO, a
// footnote's, offset] (the first wins), each `shift` further down.
static void MCAddMarkdownAnchors(NSMutableDictionary<NSString *, NSNumber *> *anchors,
                                 NSArray<NSArray *> *found, NSUInteger shift) {
    for (NSArray *a in found) {
        NSString *slug = a[1];
        const NSUInteger at = [a[2] unsignedIntegerValue] + shift;
        if ([a[0] boolValue]) {
            NSString *key = slug;
            for (int n = 1; anchors[key]; n++) key = [NSString stringWithFormat:@"%@-%d", slug, n];
            if (key.length) anchors[key] = @(at);
        } else if (slug.length && !anchors[slug]) {
            anchors[slug] = @(at);
        }
    }
}

// Runs [from, to) as page text. They start a line with nothing carried
// over from runs before them (a whole page, or a piece of one,
// MarkdownPage.h). The places links land on go into `anchors` (see
// MCAddMarkdownAnchors), at offsets into what is returned.
- (NSMutableAttributedString *)markdownRuns:(const std::vector<MdRun> &)runs
                                       from:(size_t)from
                                         to:(size_t)to
                                    anchors:(NSMutableArray<NSArray *> *)anchors {
    MCMarkdownMermaid *mermaid = [self markdownMermaid];
    NSString *mermaidComplaint = nil, *mermaidKey = nil;  // of the block being shown as code
    size_t mermaidEnd = from;
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    // A paragraph's look comes from its first run, and every run in it gets
    // that paragraph style; a quote's or a code block's paragraphs share one
    // text block, which draws the bar or the box around all of them.
    NSParagraphStyle *para = nil;
    NSMutableArray<NSTextBlock *> *quoteBlocks = [NSMutableArray array];   // one a level
    NSTextBlock *codeBox = nil;
    BOOL lineStart = YES;
    MCMarkdownCodeBlocks *codeColors = [MCMarkdownCodeBlocks new];   // ```python and the like
    for (size_t i = from; i < to;) {
        const MdRun &r = runs[i];
        if (r.tableId > 0) {
            size_t end = i;
            while (end < to && runs[end].tableId == r.tableId) end++;
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
                while (mermaidEnd < to && MermaidDiagram::isMermaidRun(runs[mermaidEnd]))
                    mermaidEnd++;
            }
        }
        if (lineStart) {
            if (r.heading > 0) {
                // A heading's text may be several runs (bold, code, links).
                std::string title;
                for (size_t k = i; k < to && runs[k].heading > 0 && runs[k].line == r.line; k++)
                    title += runs[k].text;
                // The core's anchor, so links land where they do on Linux and Android.
                const std::string anchor = MarkdownParser::anchor(title);
                NSString *slug = [[NSString alloc] initWithBytes:anchor.data() length:anchor.size()
                                                        encoding:NSUTF8StringEncoding] ?: @"";
                [anchors addObject:@[@YES, slug, @(out.length)]];
            }
            para = [self markdownParagraphFor:r quotes:quoteBlocks code:&codeBox];
        }
        if (!r.anchor.empty()) {   // a footnote, or a reference to one, that links land on
            NSString *key = [NSString stringWithUTF8String:r.anchor.c_str()];
            if (key.length) [anchors addObject:@[@NO, key, @(out.length)]];
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
    return out;
}

// The line at the top of a page with math, when it cannot be typeset or is
// still being typeset (else the TeX standing in looks like math that will
// never be), or nil. Asked once the page's runs are built.
- (NSAttributedString *)markdownNoteForRuns:(const std::vector<MdRun> &)runs {
    BOOL hasMath = NO;
    for (const MdRun &r : runs)
        if (r.math) {
            hasMath = YES;
            break;
        }
    return !hasMath ? nil
         : self.mathNoteWanted ? [self mathNote]
         : _mathRunProblem ? [self mathNoteForProblem:_mathRunProblem]
         : _awaitedMath.count ? [self mathNoteTypesetting:_awaitedMath.count]
         : nil;
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
    _sidePreview.onRender = ^{ [weakSelf renderSidePreviewAfterTyping]; };
    _sidePreview.onLink = ^BOOL(id link, NSUInteger index) {
        return [weakSelf sidePreviewClickedLink:link atIndex:index];
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

// The page for the buffer as it is now, built whole, at its top.
- (void)renderSidePreview {
    MCMarkdownSidePreview *side = [self sidePreview];
    [side cancelRender];
    side.pageState = nil;
    [self renderSidePreviewFrom:nil keepingPlace:NO];
}

// The same, keeping the page's place (a formula, a picture or a diagram
// arrived, the zoom changed, an edit made from the page) and then following
// the source unless the user had scrolled the page last; the pieces the
// page has already are kept (renderSidePreviewFrom:keepingPlace:).
- (void)renderSidePreviewKeepingPlace {
    if (!_sidePreview) return;
    MCMarkdownSidePreview *side = _sidePreview;
    __weak EditorController *weakSelf = self;
    [side holdFollowing:^{ [weakSelf renderSidePreviewFrom:nil keepingPlace:YES]; }];
    [side pageRendered];
}

// A pause in typing: the source is read on a queue of its own (the parse
// and its pieces, MarkdownPage.h), and on the main thread only the pieces
// that changed are built and put in. A render whose text is no longer the
// buffer's is dropped; the change since has scheduled another.
- (void)renderSidePreviewAfterTyping {
    MCMarkdownSidePreview *side = _sidePreview;
    if (!side || ![self sidePreviewShowing]) return;
    NSString *text = self.sourceText ?: @"";
    const NSUInteger generation = ++_sidePageGeneration;
    // The page on show, to pair the new pieces with there too: its pieces do
    // not change once made, and which may be kept is read here.
    MCSidePage *old = [side.pageState isKindOfClass:MCSidePage.class] ? side.pageState : nil;
    __block std::vector<bool> keep(old.info.count);
    for (NSUInteger k = 0; k < old.info.count; k++) keep[k] = old.info[k].keep;
    __weak EditorController *weakSelf = self;
    dispatch_async(MCSidePageQueue(), ^{
        MCSidePage *parsed = MCSidePageParse(text);
        if (old) {
            parsed->match = MarkdownPage::match(old->pieces, keep, parsed->pieces);
            parsed.matchedTo = old;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            EditorController *me = weakSelf;
            if (!me || generation != me->_sidePageGeneration || me.sourceText != text ||
                ![me sidePreviewShowing] || me->_sidePreview != side)
                return;
            const double c0 = MCThreadCpuMs();
            [side holdFollowing:^{ [me renderSidePreviewFrom:parsed keepingPlace:YES]; }];
            [side pageRendered];
            side.lastRenderCpuMilliseconds = MCThreadCpuMs() - c0;
        });
    });
}

// What a piece's text depends on besides its runs: a page built for other
// ones is built again whole.
- (NSString *)sidePageContextFor:(MCMarkdownSidePreview *)side {
    const Settings &cfg = [AppSettings shared].settings;
    return [NSString stringWithFormat:@"%@|%.2f|%.3f|%d%d%d%d%d", self.currentPath ?: @"",
                                      side.pageWidth, MCZoomScale(), cfg.isDark(),
                                      [self typesetsMath], cfg.mermaid(), cfg.webImages(),
                                      cfg.math()];
}

// Piece k of `page` as text, its source lines as MCMarkdownLine objects
// (one a line), its anchors, and whether it may be kept as it is (it waits
// for nothing: no picture on its way, no formula or diagram still out).
- (MCSidePiece *)buildSidePiece:(MCSidePage *)page
                          index:(size_t)k
                           text:(NSMutableAttributedString **)textOut {
    const MarkdownPage::Piece &piece = page->pieces[k];
    MCSidePiece *info = [MCSidePiece new];
    NSMutableArray<NSArray *> *anchors = [NSMutableArray array];
    const NSUInteger pictures = _webImageMarks, formulas = _mathMarks;
    NSMutableAttributedString *text = [self markdownRuns:page->runs from:piece.begin to:piece.end
                                                 anchors:anchors];
    BOOL keep = _webImageMarks == pictures && _mathMarks == formulas;
    if (keep && _mathRunProblem)
        for (size_t i = piece.begin; i < piece.end && keep; i++) keep = !page->runs[i].math;
    // Each line's runs share one object, whose number a later render moves.
    std::vector<std::pair<NSRange, int>> lined;
    std::vector<std::pair<NSRange, int>> *out = &lined;
    const NSRange all = NSMakeRange(0, text.length);
    [text enumerateAttribute:kMarkdownSourceLine inRange:all options:0
                  usingBlock:^(id v, NSRange r, BOOL *stop) {
        (void)stop;
        const int line = MCMarkdownLineOf(v);
        if (line >= 0) out->push_back({r, line});
    }];
    NSMutableArray<MCMarkdownLine *> *lines = [NSMutableArray array];
    std::unordered_map<int, MCMarkdownLine *> byLine;   // a long code block is one piece
    for (const auto &l : lined) {
        MCMarkdownLine *__strong &o = byLine[l.second];
        if (!o) {
            o = [[MCMarkdownLine alloc] initWithLine:l.second];
            [lines addObject:o];
        }
        [text addAttribute:kMarkdownSourceLine value:o range:l.first];
    }
    if (keep) {
        __block BOOL waiting = NO;
        [text enumerateAttribute:MCMarkdownMermaidAttribute inRange:all options:0
                      usingBlock:^(id v, NSRange r, BOOL *stop) {
            (void)r;
            if (v) waiting = *stop = YES;
        }];
        keep = !waiting;
    }
    info.length = text.length;
    info.base = piece.base;
    info.lines = lines;
    info.anchors = anchors;
    info.keep = keep;
    *textOut = text;
    return info;
}

// Puts the page for the buffer on show: `parsed` (from the queue) or the
// buffer read now. A page of the same context keeps every piece that is
// the same as before, its line numbers moved, and only the stretches
// between kept pieces are replaced in the text, each a storage edit of its
// own, so TextKit lays out again only from the first one and only as far as
// it is asked; the place on show is kept by the character at the top,
// moved by what the edits above it changed. Without such a page (the
// first, another width, zoom, theme or settings) the page is built whole.
- (void)renderSidePreviewFrom:(MCSidePage *)parsed keepingPlace:(BOOL)keepPlace {
    MCMarkdownSidePreview *side = [self sidePreview];
    [side cancelRender];
    NSString *text = self.sourceText ?: @"";
    if (!parsed) parsed = MCSidePageParse(text);
    NSTextView *tv = side.textView;
    NSTextStorage *st = tv.textStorage;
    MCSidePage *previous = [side.pageState isKindOfClass:MCSidePage.class] ? side.pageState : nil;
    MCSidePage *old = previous;
    NSString *context = [self sidePageContextFor:side];
    if (![old.context isEqualToString:context] || st.length != old.length) old = nil;
    parsed.context = context;
    [self beginMarkdownPageWidth:side.pageWidth];

    const size_t n = parsed->pieces.size();
    std::vector<long> match(n, -1);
    if (old && parsed.matchedTo == old && parsed->match.size() == n) {
        match = parsed->match;   // paired on the queue
    } else if (old) {
        std::vector<bool> keep(old.info.count);
        for (NSUInteger k = 0; k < old.info.count; k++) keep[k] = old.info[k].keep;
        match = MarkdownPage::match(old->pieces, keep, parsed->pieces);
    }
    NSMutableArray<MCSidePiece *> *info = [NSMutableArray arrayWithCapacity:n];
    NSMutableArray *built = [NSMutableArray arrayWithCapacity:n];   // text, or NSNull when kept
    for (size_t k = 0; k < n; k++) {
        if (match[k] >= 0) {
            MCSidePiece *p = old.info[(NSUInteger)match[k]];
            [p moveToBase:parsed->pieces[k].base];
            [info addObject:p];
            [built addObject:NSNull.null];
            continue;
        }
        NSMutableAttributedString *t = nil;
        [info addObject:[self buildSidePiece:parsed index:k text:&t]];
        [built addObject:t];
    }
    NSAttributedString *note = [self markdownNoteForRuns:parsed->runs] ?: [NSAttributedString new];
    parsed.info = info;
    parsed.noteLength = note.length;

    if (!old || ![self spliceSidePage:side old:old note:note built:built match:match]) {
        // Whole: where the page's top was, kept by its character.
        NSUInteger anchor = NSNotFound;
        CGFloat offset = 0;
        if (keepPlace && side.pageLeads && side.scrollView.contentView.bounds.origin.y > 0 &&
            st.length)
            anchor = MCPageTopCharacter(tv, &offset);
        const NSUInteger oldNote = previous ? previous.noteLength : 0;
        NSMutableAttributedString *page = [note mutableCopy];
        for (NSUInteger k = 0; k < n; k++) {
            id t = built[k];
            if (t == NSNull.null) {   // kept, but the page to keep it from did not add up
                NSMutableAttributedString *fresh = nil;
                info[k] = [self buildSidePiece:parsed index:k text:&fresh];
                t = fresh;
            }
            [page appendAttributedString:t];
        }
        [side showPage:page];
        if (anchor != NSNotFound && page.length) {
            anchor = anchor >= oldNote ? anchor - oldNote + note.length : 0;
            const NSRect r = MCTextViewRectForCharacter(tv, MIN(anchor, page.length - 1));
            if (!NSIsEmptyRect(r)) MCScrollTextViewToY(side.scrollView, tv, NSMinY(r) - offset);
        }
    }
    parsed.length = st.length;
    // The places links land on, piece by piece down the page.
    NSMutableDictionary<NSString *, NSNumber *> *anchors = [NSMutableDictionary dictionary];
    NSUInteger at = note.length;
    for (MCSidePiece *p in info) {
        if (p.anchors.count) MCAddMarkdownAnchors(anchors, p.anchors, at);
        at += p.length;
    }
    side.anchors = anchors;
    parsed.matchedTo = nil;
    side.pageState = parsed;
    side.renderedSource = text;
    [self finishMarkdownPage];
    // The runs are needed no more (the pieces' hashes are what the next
    // page is matched by): freed off the main thread, as the last page is
    // (a long page's take milliseconds).
    auto runs = std::make_shared<std::vector<MdRun>>(std::move(parsed->runs));
    parsed->runs.clear();
    dispatch_async(MCSidePageQueue(), ^{
        (void)previous;
        (void)runs;
    });
}

// The page on show (built from `old`) brought to the new pieces: the note
// at the top, then each stretch of new pieces between kept ones in place
// of the old pieces between them. NO, changing nothing, if the old page's
// lengths do not add up to what is on show.
- (BOOL)spliceSidePage:(MCMarkdownSidePreview *)side
                   old:(MCSidePage *)old
                  note:(NSAttributedString *)note
                 built:(NSArray *)built
                 match:(const std::vector<long> &)match {
    NSTextView *tv = side.textView;
    NSTextStorage *st = tv.textStorage;
    const NSUInteger count = old.info.count;
    std::vector<NSUInteger> starts(count + 1);
    starts[0] = old.noteLength;
    for (NSUInteger k = 0; k < count; k++) starts[k + 1] = starts[k] + old.info[k].length;
    if (starts[count] != st.length) return NO;

    struct Edit {
        NSRange range;
        NSAttributedString *with;
    };
    std::vector<Edit> edits;
    NSAttributedString *oldNote =
        old.noteLength ? [st attributedSubstringFromRange:NSMakeRange(0, old.noteLength)] : nil;
    if (!(oldNote ? [oldNote isEqualToAttributedString:note] : note.length == 0))
        edits.push_back({NSMakeRange(0, old.noteLength), note});
    NSMutableAttributedString *pending = nil;
    long previous = -1;
    auto flush = [&](long next) {
        const NSUInteger from = starts[(size_t)(previous + 1)], to = starts[(size_t)next];
        if (pending.length || to > from)
            edits.push_back({NSMakeRange(from, to - from), pending ?: [NSAttributedString new]});
        pending = nil;
    };
    for (size_t k = 0; k < match.size(); k++) {
        if (match[k] < 0) {
            if (!pending) pending = [NSMutableAttributedString new];
            [pending appendAttributedString:built[k]];
            continue;
        }
        if (pending || match[k] != previous + 1) flush(match[k]);
        previous = match[k];
    }
    if (pending || (NSUInteger)(previous + 1) != count) flush((long)count);
    if (edits.empty()) return YES;

    // The character at the page's top, and where the edits above it move it
    // (unless the page is to follow the source, which puts it elsewhere:
    // keeping it here would lay the page out to where it was for nothing).
    CGFloat offset = 0;
    NSUInteger anchor = NSNotFound;
    if (side.pageLeads && side.scrollView.contentView.bounds.origin.y > 0 && st.length)
        anchor = MCPageTopCharacter(tv, &offset);
    BOOL moves = NO;   // something above it changed, so its height may have
    NSUInteger moved = anchor;
    if (anchor != NSNotFound) {
        long shift = 0;
        for (const Edit &e : edits) {
            if (anchor < e.range.location) break;
            moves = YES;
            if (e.range.length && anchor < NSMaxRange(e.range)) {
                // In what is replaced: as far into its new text.
                moved = (NSUInteger)((long)e.range.location + shift) +
                        MIN(anchor - e.range.location, e.with.length ? e.with.length - 1 : 0);
                shift = 0;
                anchor = NSNotFound;
                break;
            }
            shift += (long)e.with.length - (long)e.range.length;
        }
        if (anchor != NSNotFound) moved = (NSUInteger)((long)anchor + shift);
    }
    for (auto e = edits.rbegin(); e != edits.rend(); ++e)
        [st replaceCharactersInRange:e->range withAttributedString:e->with];
    [side pageChanged];
    if (moves && st.length) {
        const NSRect r = MCTextViewRectForCharacter(tv, MIN(moved, st.length - 1));
        if (!NSIsEmptyRect(r)) MCScrollTextViewToY(side.scrollView, tv, NSMinY(r) - offset);
    }
    return YES;
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
    _sidePreview.pageState = nil;   // its colors are in every piece: built again whole
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

// A link clicked in the pane (at character `index`): a heading or a
// footnote scrolls the pane, a task's box ticks the task in the source (one
// undo step, as a click in the full preview is), and the rest open as from
// the full preview. The box's line is its run's (the number in the link is
// the one it was built with, which a later render may have moved), and a
// page behind the typing is brought up to date instead of ticking a line
// that has moved.
- (BOOL)sidePreviewClickedLink:(id)link atIndex:(NSUInteger)index {
    if (![link isKindOfClass:NSString.class]) return NO;
    NSString *href = link;
    if ([href hasPrefix:kTaskLinkPrefix]) {
        if ([self catchUpSidePage]) return YES;
        NSTextStorage *page = _sidePreview.textView.textStorage;
        int line = index < page.length
                       ? MCMarkdownLineOf([page attribute:kMarkdownSourceLine atIndex:index
                                           effectiveRange:NULL])
                       : -1;
        if (line < 0) line = [[href substringFromIndex:kTaskLinkPrefix.length] intValue];
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

// ------------------------------------------------------- Export as HTML…
// The buffer as a standalone page (MarkdownHtml.h), unsaved changes
// included, written through the atomic writer where the save panel says:
// beside the file at first, so its relative pictures and links still work.

NSString * (^MCHtmlExportPathChooser)(NSString *suggested) = nil;

- (BOOL)canExportMarkdownHtml {
    return self.isMarkdown && _activeTab != nil && self.currentPath != nil && !self.showingMessage &&
           !self.isDiff;
}

- (void)exportMarkdownHtml:(id)sender {
    (void)sender;
    if (![self canExportMarkdownHtml]) {
        NSBeep();
        return;
    }
    NSString *suggested =
        [self.currentPath.stringByDeletingPathExtension stringByAppendingPathExtension:@"html"];
    NSString *path = nil;
    if (MCHtmlExportPathChooser) {
        path = MCHtmlExportPathChooser(suggested);
    } else {
        NSSavePanel *panel = [NSSavePanel savePanel];
        panel.directoryURL = [NSURL fileURLWithPath:suggested.stringByDeletingLastPathComponent];
        panel.nameFieldStringValue = suggested.lastPathComponent;
        panel.allowedFileTypes = @[@"html"];
        panel.canCreateDirectories = YES;
        if ([panel runModal] == NSModalResponseOK) path = panel.URL.path;
    }
    if (path.length) [self writeMarkdownHtmlTo:path];
}

- (BOOL)writeMarkdownHtmlTo:(NSString *)path {
    const std::string html = MarkdownHtml::page(Utf8String(self.sourceText ?: @""),
                                                Utf8String(self.currentPath.lastPathComponent ?: @""));
    std::string error;
    if (!FolderSearch::writeFileAtomically(path.fileSystemRepresentation, html, &error)) {
        [self showStatusNote:[NSString stringWithFormat:@"%@ could not be written: %@",
                                                        path.lastPathComponent, [NSString stringWithUTF8String:error.c_str()] ?: @""]];
        return NO;
    }
    [self showStatusNote:[NSString stringWithFormat:@"Exported %@", path.lastPathComponent]];
    return YES;
}

@end
