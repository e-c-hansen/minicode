// CompareView.mm — see CompareView.h.
#import "CompareView.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Appearance.h"
#include "SideBySide.h"
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <string>
#include <vector>

static NSColor *CHex(unsigned rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

static std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

static NSString *NSFromU16(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

// Past this many UTF-16 units a side, the panes are not syntax colored:
// coloring is linear, but a run per token in the attributed string is not
// free, and nobody reads a 2 MB diff for its colors.
static const size_t kMaxColoredText = 2 * 1024 * 1024;

// The syntax colors of one side, as spans over its text.
static std::vector<SideBySide::Span> MCSyntaxSpans(const std::u16string &text,
                                                   const std::string &ext) {
    std::vector<SideBySide::Span> out;
    if (ext.empty() || text.empty() || text.size() > kMaxColoredText ||
        !SyntaxHighlighter::supports(ext))
        return out;
    IncrementalHighlighter<char16_t> hl(ext);
    std::vector<Token> tokens;
    hl.reset(StringSource<char16_t>(text), &tokens);
    out.reserve(tokens.size());
    for (const Token &t : tokens)
        if (t.style != TokenStyle::Plain && t.length)
            out.push_back({t.start, t.length, (int)t.style});
    return out;
}

// ---------------------------------------------------------------- the model
// Posted (object: the comparison) when its decision changes, a hunk's
// included, so the view showing it redraws its bar, gutter and tints.
static NSNotificationName const MCComparisonDecisionChanged = @"MCComparisonDecisionChanged";

// What prepare works out for one pane.
struct MCPane {
    std::u16string text;
    std::vector<size_t> starts;            // each row's first character
    std::vector<SideBySide::Span> syntax;  // style: a TokenStyle
    std::vector<SideBySide::Span> words;   // the changed words of changed rows
    size_t columns = 0;                    // the longest row, a tab as 8
};

// The panes' font, and the height every row is given (a paragraph style
// holds it, so a line drawn in a fallback font, an emoji's, is no taller
// and the panes stay level).
static NSFont *MCCompareFont() { return MCMonoFont(12.5, NSFontWeightRegular); }
static CGFloat MCCompareRowHeight() {
    static CGFloat h;
    if (!h) {
        NSLayoutManager *lm = [NSLayoutManager new];
        h = ceil([lm defaultLineHeightForFont:MCCompareFont()]);
    }
    return h;
}

@interface MCComparison ()
- (const std::vector<SideBySide::Row> &)rows;
- (const std::vector<SideBySide::Hunk> &)hunks;
- (const MCPane &)pane:(BOOL)left;
@end

@implementation MCComparison {
    std::vector<SideBySide::Row> _rows;
    std::vector<SideBySide::Hunk> _hunks;
    std::vector<signed char> _choices;   // per hunk: 1, -1, or 0 undecided
    MCPane _panes[2];                    // left, right
    NSUInteger _changed;
    BOOL _prepared;
}
- (void)setLeftText:(NSString *)text {
    _leftText = [text copy];
    _prepared = NO;
}
- (void)setRightText:(NSString *)text {
    _rightText = [text copy];
    _prepared = NO;
}
- (void)setFileName:(NSString *)name {
    _fileName = [name copy];
    _prepared = NO;
}
// The alignment and what the panes show, once: it is the costly part of a
// big comparison, and both the count and the view need it.
- (void)prepare {
    if (_prepared) return;
    const std::u16string a = U16(_leftText ?: @""), b = U16(_rightText ?: @"");
    _rows = SideBySide::align(a, b);
    _hunks = SideBySide::hunks(_rows);
    _choices.assign(_hunks.size(), 0);
    _changed = 0;
    for (const SideBySide::Row &r : _rows)
        if (r.kind != SideBySide::Kind::Same) _changed++;
    NSString *name = _fileName.length ? _fileName : _path;
    const std::string ext = name.pathExtension.lowercaseString.UTF8String ?: "";
    for (int side = 0; side < 2; side++) {
        const bool left = side == 0;
        const std::u16string &text = left ? a : b;
        MCPane &p = _panes[side];
        p.text = SideBySide::paneText(text, _rows, left, &p.starts);
        p.syntax = SideBySide::spansOnPane(text, _rows, left, p.starts, MCSyntaxSpans(text, ext));
        p.words.clear();
        p.columns = 0;
        size_t col = 0;
        for (char16_t ch : p.text) {
            if (ch == u'\n') col = 0;
            else col += ch == u'\t' ? 8 : 1;
            p.columns = std::max(p.columns, col);
        }
    }
    // The words that changed, row by row, in pane offsets. A line longer
    // than a screen is left to its tint, and past a few megabytes of
    // changed lines the rest are too: nobody reads that word by word, and
    // it keeps a huge rewrite from taking seconds here.
    size_t budget = 4 * 1024 * 1024;
    for (size_t r = 0; r < _rows.size() && budget > 0; r++) {
        if (_rows[r].kind != SideBySide::Kind::Changed) continue;
        auto line = [&](const MCPane &p) {
            const size_t from = p.starts[r];
            const size_t to = r + 1 < p.starts.size() ? p.starts[r + 1] - 1 : p.text.size();
            return p.text.substr(from, to - from);
        };
        const std::u16string oldLine = line(_panes[0]), newLine = line(_panes[1]);
        if (oldLine.size() > 2000 || newLine.size() > 2000) continue;
        budget -= std::min(budget, oldLine.size() + newLine.size());
        const SideBySide::WordChanges w = SideBySide::wordChanges(oldLine, newLine);
        for (const SideBySide::Span &s : w.left)
            _panes[0].words.push_back({_panes[0].starts[r] + s.start, s.length, 0});
        for (const SideBySide::Span &s : w.right)
            _panes[1].words.push_back({_panes[1].starts[r] + s.start, s.length, 0});
    }
    _prepared = YES;
}
- (const std::vector<SideBySide::Row> &)rows {
    [self prepare];
    return _rows;
}
- (const std::vector<SideBySide::Hunk> &)hunks {
    [self prepare];
    return _hunks;
}
- (const MCPane &)pane:(BOOL)left {
    [self prepare];
    return _panes[left ? 0 : 1];
}
- (NSUInteger)changedRows {
    [self prepare];
    return _changed;
}
- (NSUInteger)hunkCount {
    [self prepare];
    return _hunks.size();
}
- (NSInteger)decisionOfHunk:(NSUInteger)hunk {
    [self prepare];
    return hunk < _choices.size() ? _choices[hunk] : 0;
}
- (BOOL)hunksDecided {
    for (signed char c : _choices)
        if (c) return YES;
    return NO;
}
- (void)changed {
    [[NSNotificationCenter defaultCenter] postNotificationName:MCComparisonDecisionChanged
                                                        object:self];
}
- (void)decideHunk:(NSUInteger)hunk as:(NSInteger)choice {
    [self prepare];
    if (!_decisionPrompt || _decision != 0 || hunk >= _choices.size()) return;
    _choices[hunk] = (signed char)(choice > 0 ? 1 : choice < 0 ? -1 : 0);
    for (signed char c : _choices)
        if (!c) {
            [self changed];
            return;
        }
    [self finish];
}
- (void)decideRest:(BOOL)accepted {
    [self prepare];
    if (!_decisionPrompt || _decision != 0) return;
    for (signed char &c : _choices)
        if (!c) c = accepted ? 1 : -1;
    // With no hunks (the texts differ only in the last line's "\n") the
    // button says it all.
    if (_choices.empty()) _decision = accepted ? 1 : -1;
    [self finish];
}
- (void)decide:(BOOL)accepted {
    [self prepare];
    if (!_decisionPrompt || _decision != 0) return;
    if (!accepted) _choices.assign(_choices.size(), -1);
    [self decideRest:accepted];
}
// Every hunk has its choice: the answer is the accepted ones, or a
// rejection when there are none.
- (void)finish {
    size_t taken = 0;
    for (signed char c : _choices) taken += c > 0;
    if (!_choices.empty()) _decision = taken ? 1 : -1;
    if (_decision == 1) {
        if (taken == _choices.size()) {
            _acceptedText = [_rightText copy] ?: @"";
        } else {
            std::vector<bool> pick(_choices.size());
            for (size_t i = 0; i < pick.size(); i++) pick[i] = _choices[i] > 0;
            _acceptedText = NSFromU16(SideBySide::assemble(U16(_leftText ?: @""),
                                                           U16(_rightText ?: @""), _rows, pick));
        }
    }
    void (^done)(BOOL) = _onDecision;
    _onDecision = nil;
    [self changed];
    if (done) done(_decision == 1);
}
- (void)endDecision:(NSString *)why {
    if (!_decisionPrompt || _decision != 0) return;
    _decision = 2;
    _decisionNote = [why copy];
    _onDecision = nil;
    [self changed];
}
- (void)tabClosed {
    [self decide:NO];
}
@end

// ------------------------------------------------------------- one pane
// A read-only text view that draws, behind its text, each row's tint and,
// in its left margin, the row's line number (none on a filler row).
@interface MCCompareTextView : NSTextView
@property(nonatomic) BOOL leftSide;
@property(nonatomic, strong) NSColor *numberColor;
- (void)setRows:(const std::vector<SideBySide::Row> &)rows starts:(std::vector<size_t>)starts;
// Per row: 0, or a hunk's choice (1 accepted, -1 rejected), which tints it.
- (void)setRowChoices:(std::vector<signed char>)choices;
- (size_t)rowAtCharacter:(NSUInteger)index;
- (NSUInteger)startOfRow:(size_t)row;
// Row `row`'s line in this view's coordinates.
- (NSRect)rectOfRow:(size_t)row;
@end

static const CGFloat kGutter = 52;

@implementation MCCompareTextView {
    std::vector<SideBySide::Row> _rows;
    std::vector<size_t> _starts;   // each row's first character
    std::vector<signed char> _choices;
}

- (void)setRows:(const std::vector<SideBySide::Row> &)rows starts:(std::vector<size_t>)starts {
    _rows = rows;
    _starts = std::move(starts);
    _choices.assign(_rows.size(), 0);
    self.needsDisplay = YES;
}

- (void)setRowChoices:(std::vector<signed char>)choices {
    _choices = std::move(choices);
    self.needsDisplay = YES;
}

- (size_t)rowAtCharacter:(NSUInteger)index {
    auto it = std::upper_bound(_starts.begin(), _starts.end(), (size_t)index);
    return it == _starts.begin() ? 0 : (size_t)(it - _starts.begin() - 1);
}

- (NSUInteger)startOfRow:(size_t)row {
    return row < _starts.size() ? _starts[row] : self.string.length;
}

// Every row is one line of the same height, so its place is arithmetic:
// nothing is laid out to find it.
- (NSRect)rectOfRow:(size_t)row {
    const CGFloat h = MCCompareRowHeight();
    return NSMakeRect(0, self.textContainerOrigin.y + (CGFloat)row * h, NSWidth(self.bounds), h);
}

- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    if (_rows.empty()) return;
    NSDictionary *numAttrs = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: self.numberColor ?: [NSColor grayColor]};
    NSColor *removed = CHex(0xF14C4C, 0.16), *added = CHex(0x73C991, 0.16);
    NSColor *filler = [NSColor colorWithWhite:0.5 alpha:0.07];
    // A hunk the user rejected keeps only a trace of its tint.
    NSColor *dropped = [NSColor colorWithWhite:0.5 alpha:0.10];
    auto drawRow = [&](size_t r) {
        const NSRect line = [self rectOfRow:r];
        if (!NSIntersectsRect(line, rect)) return;
        const SideBySide::Row &row = _rows[r];
        const int number = self.leftSide ? row.left : row.right;
        const signed char choice = r < _choices.size() ? _choices[r] : 0;
        NSColor *tint = nil;
        if (number < 0) tint = filler;
        else if (row.kind != SideBySide::Kind::Same)
            tint = choice < 0 && !self.leftSide ? dropped : self.leftSide ? removed : added;
        if (tint) {
            [tint set];
            NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
        }
        if (number >= 0) {
            NSString *n = [NSString stringWithFormat:@"%d", number + 1];
            NSSize sz = [n sizeWithAttributes:numAttrs];
            [n drawAtPoint:NSMakePoint(kGutter - 12 - sz.width,
                                       NSMinY(line) + (NSHeight(line) - sz.height) / 2)
            withAttributes:numAttrs];
        }
    };
    // The rows the rect crosses, by arithmetic (rows are all one height),
    // so nothing is laid out for the tints and numbers.
    const CGFloat h = MCCompareRowHeight(), top = self.textContainerOrigin.y;
    const long first = std::max(0L, (long)floor((NSMinY(rect) - top) / h));
    const long last = std::min((long)_rows.size() - 1, (long)floor((NSMaxY(rect) - top) / h));
    for (long r = first; r <= last; r++) drawRow((size_t)r);
}
@end

// ------------------------------------------- the hunk buttons between panes
@class MCCompareHunkGutter;
@interface MCCompareView () <NSMenuItemValidation>
@property(nonatomic, strong) NSTextField *leftHeader, *rightHeader, *changesLabel;
@property(nonatomic, strong) NSView *divider;
// The bar over the panes while a change waits for a decision.
@property(nonatomic, strong) NSView *decisionBar;
@property(nonatomic, strong) NSTextField *decisionLabel;
@property(nonatomic, strong) MCCompareHunkGutter *hunkGutter;
- (void)drawHunkButtonsIn:(NSView *)gutter;
@end

// Draws the Accept and Reject buttons of each hunk on screen, and takes
// clicks on them; the compare view knows where they are.
@interface MCCompareHunkGutter : NSView
@property(nonatomic, weak) MCCompareView *owner;
@end

@implementation MCCompareHunkGutter
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [self.owner drawHunkButtonsIn:self];
}
- (void)mouseDown:(NSEvent *)e {
    MCCompareView *owner = self.owner;
    [owner pressHunkButtonAtPoint:[owner convertPoint:e.locationInWindow fromView:nil]];
}
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
@end

// ---------------------------------------------------------- the two panes
static const CGFloat kHunkGutterWidth = 46, kHunkButton = 17;

@implementation MCCompareView {
    MCCompareTextView *_left, *_right;
    NSScrollView *_leftScroll, *_rightScroll;
    BOOL _syncing;
    NSInteger _currentHunk;
}

- (NSScrollView *)makePane:(MCCompareTextView *__strong *)out left:(BOOL)left {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.automaticallyAdjustsContentInsets = NO;
    // TextKit 1, built by hand: the rows are drawn from its line fragments.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *lm = [NSLayoutManager new];
    [storage addLayoutManager:lm];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(FLT_MAX, FLT_MAX)];
    tc.widthTracksTextView = NO;
    [lm addTextContainer:tc];
    lm.allowsNonContiguousLayout = YES;
    MCCompareTextView *tv = [[MCCompareTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)
                                                       textContainer:tc];
    tv.leftSide = left;
    tv.editable = NO;
    tv.selectable = YES;
    tv.richText = NO;
    tv.drawsBackground = NO;
    tv.usesFontPanel = NO;
    tv.usesFindBar = YES;
    tv.incrementalSearchingEnabled = YES;
    tv.textContainerInset = NSMakeSize(kGutter, 6);
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    // Sized by fitTextViews alone; lines never wrap, so rows stay level.
    tv.verticallyResizable = NO;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewNotSizable;
    scroll.documentView = tv;
    scroll.contentView.postsBoundsChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(paneScrolled:)
                                                 name:NSViewBoundsDidChangeNotification
                                               object:scroll.contentView];
    *out = tv;
    return scroll;
}

- (NSTextField *)makeHeader {
    NSTextField *f = [NSTextField labelWithString:@""];
    f.font = [NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium];
    f.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return f;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        _currentHunk = -1;
        _leftScroll = [self makePane:&_left left:YES];
        _rightScroll = [self makePane:&_right left:NO];
        [self addSubview:_leftScroll];
        [self addSubview:_rightScroll];
        self.leftHeader = [self makeHeader];
        self.rightHeader = [self makeHeader];
        self.changesLabel = [self makeHeader];
        self.changesLabel.alignment = NSTextAlignmentRight;
        self.changesLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightRegular];
        [self addSubview:self.leftHeader];
        [self addSubview:self.rightHeader];
        [self addSubview:self.changesLabel];
        self.divider = [[NSView alloc] initWithFrame:NSZeroRect];
        self.divider.wantsLayer = YES;
        self.divider.layer.backgroundColor = CHex(0x333333, 1).CGColor;
        [self addSubview:self.divider];
        self.hunkGutter = [[MCCompareHunkGutter alloc] initWithFrame:NSZeroRect];
        self.hunkGutter.owner = self;
        self.hunkGutter.hidden = YES;
        [self addSubview:self.hunkGutter];
        [self makeDecisionBar];
        [self applySettings];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(applySettings)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)makeDecisionBar {
    self.decisionBar = [[NSView alloc] initWithFrame:NSZeroRect];
    self.decisionBar.wantsLayer = YES;
    self.decisionLabel = [NSTextField labelWithString:@""];
    self.decisionLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    self.decisionLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _acceptButton = [NSButton buttonWithTitle:@"Accept" target:self action:@selector(acceptChange:)];
    _rejectButton = [NSButton buttonWithTitle:@"Reject" target:self action:@selector(rejectChange:)];
    for (NSButton *b in @[_acceptButton, _rejectButton]) {
        b.bezelStyle = NSBezelStyleRounded;
        b.controlSize = NSControlSizeSmall;
        b.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        [self.decisionBar addSubview:b];
    }
    _acceptButton.bezelColor = CHex(0x2E7D4F, 1);
    [self.decisionBar addSubview:self.decisionLabel];
    self.decisionBar.hidden = YES;
    [self addSubview:self.decisionBar];
}

// The bar's buttons: the whole change, or once some hunks have their own
// choice, the hunks still undecided.
- (void)acceptChange:(id)sender { (void)sender; [self.comparison decideRest:YES]; }
- (void)rejectChange:(id)sender {
    (void)sender;
    MCComparison *c = self.comparison;
    if (c.hunksDecided) [c decideRest:NO];
    else [c decide:NO];
}

- (void)decisionChanged:(NSNotification *)note {
    if (note.object == self.comparison) [self showDecision];
}

// Whether the per-hunk buttons show: a decision pending over hunks.
- (BOOL)hunkButtonsWanted {
    MCComparison *c = self.comparison;
    return c.decisionPrompt.length > 0 && c.decision == 0 && c.hunkCount > 0;
}

// The bar's words and buttons for the comparison's decision.
- (void)showDecision {
    MCComparison *c = self.comparison;
    const BOOL wanted = c.decisionPrompt.length > 0;
    const BOOL gutter = [self hunkButtonsWanted];
    const BOOL changed = self.decisionBar.hidden == wanted || self.hunkGutter.hidden == gutter;
    self.decisionBar.hidden = !wanted;
    self.hunkGutter.hidden = !gutter;
    if (wanted) {
        NSString *text = c.decisionPrompt;
        const NSUInteger n = c.hunkCount;
        NSUInteger yes = 0, no = 0;
        for (NSUInteger i = 0; i < n; i++) {
            const NSInteger d = [c decisionOfHunk:i];
            yes += d > 0;
            no += d < 0;
        }
        if (c.decision == 1) {
            text = yes < n ? [NSString stringWithFormat:@"Accepted %lu of %lu changes: Claude Code writes them.",
                                                        (unsigned long)yes, (unsigned long)n]
                           : @"Accepted: Claude Code writes the change.";
        } else if (c.decision == -1) {
            text = @"Rejected.";
        } else if (c.decision == 2) {
            text = c.decisionNote ?: @"No longer waiting for an answer.";
        } else if (yes || no) {
            text = [NSString stringWithFormat:@"%@ %lu accepted, %lu rejected, %lu to decide.", text,
                                              (unsigned long)yes, (unsigned long)no,
                                              (unsigned long)(n - yes - no)];
        }
        self.decisionLabel.stringValue = text;
        self.decisionLabel.toolTip = text;
        _acceptButton.hidden = _rejectButton.hidden = c.decision != 0;
        const BOOL rest = c.hunksDecided;
        _acceptButton.title = rest ? @"Accept the Rest" : @"Accept";
        _rejectButton.title = rest ? @"Reject the Rest" : @"Reject";
        _acceptButton.toolTip = rest ? @"Accept every change not yet decided" : @"Accept the whole change";
        _rejectButton.toolTip = rest ? @"Reject every change not yet decided" : @"Reject the whole change";
    }
    [self applyRowChoices];
    if (changed) [self layoutPanes];
    else [self layoutDecisionBar];
}

// Each row of a decided hunk is tinted by its choice.
- (void)applyRowChoices {
    MCComparison *c = self.comparison;
    std::vector<signed char> rowChoices;
    if (c) {
        const std::vector<SideBySide::Hunk> &hs = [c hunks];
        rowChoices.assign([c rows].size(), 0);
        for (size_t i = 0; i < hs.size(); i++) {
            const signed char d = (signed char)[c decisionOfHunk:i];
            for (size_t r = hs[i].first; r < hs[i].end && r < rowChoices.size(); r++) rowChoices[r] = d;
        }
    }
    [_left setRowChoices:rowChoices];
    [_right setRowChoices:rowChoices];
    self.hunkGutter.needsDisplay = YES;
}

- (void)layoutDecisionBar {
    const NSRect b = self.bounds;
    const CGFloat h = self.decisionBar.hidden ? 0 : 34;
    self.decisionBar.frame = NSMakeRect(0, 0, NSWidth(b), h);
    if (!h) return;
    [_acceptButton sizeToFit];
    [_rejectButton sizeToFit];
    CGFloat x = NSWidth(b) - 10;
    for (NSButton *btn in @[_rejectButton, _acceptButton]) {
        if (btn.hidden) continue;
        const NSSize sz = btn.frame.size;
        x -= sz.width;
        btn.frame = NSMakeRect(x, floor((h - sz.height) / 2), sz.width, sz.height);
        x -= 8;
    }
    self.decisionLabel.frame = NSMakeRect(12, floor((h - 16) / 2), MAX(0, x - 12), 16);
}

- (BOOL)isFlipped { return YES; }
- (NSTextView *)leftView { return _left; }
- (NSTextView *)rightView { return _right; }
- (NSScrollView *)leftScroll { return _leftScroll; }
- (NSScrollView *)rightScroll { return _rightScroll; }
- (NSInteger)currentHunk { return _currentHunk; }
- (NSString *)changesText { return self.changesLabel.stringValue; }

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self layoutPanes];
}

- (void)layoutPanes {
    [self layoutDecisionBar];
    const NSRect b = self.bounds;
    const CGFloat top = NSHeight(self.decisionBar.frame);
    const CGFloat headerH = 24;
    const CGFloat g = self.hunkGutter.hidden ? 1 : kHunkGutterWidth;
    const CGFloat half = floor((NSWidth(b) - g) / 2);
    const CGFloat rightX = half + g, rightW = MAX(0, NSWidth(b) - rightX);
    const CGFloat paneH = MAX(0, NSHeight(b) - top - headerH);
    self.leftHeader.frame = NSMakeRect(10, top + 4, MAX(0, half - 20), 16);
    const CGFloat countW = MIN(130, rightW / 2);
    self.rightHeader.frame = NSMakeRect(rightX + 10, top + 4, MAX(0, rightW - 30 - countW), 16);
    self.changesLabel.frame = NSMakeRect(NSWidth(b) - 10 - countW, top + 4, countW, 16);
    _leftScroll.frame = NSMakeRect(0, top + headerH, half, paneH);
    _rightScroll.frame = NSMakeRect(rightX, top + headerH, rightW, paneH);
    self.divider.frame = NSMakeRect(half, top, 1, MAX(0, NSHeight(b) - top));
    self.divider.hidden = !self.hunkGutter.hidden;
    self.hunkGutter.frame = NSMakeRect(half, top + headerH, g, paneH);
    [self fitTextViews];
}

// Each text view as wide as its longest line or its pane, whichever is more,
// and as tall as its rows: from the font's measures, never by laying the
// text out (which took 1.5 s for two panes of 50,000 lines). Layout is
// non-contiguous, so only what is drawn gets laid out.
- (void)fitTextViews {
    static const MCPane empty;
    const CGFloat advance = [@"0" sizeWithAttributes:@{NSFontAttributeName: MCCompareFont()}].width;
    const size_t rows = self.comparison ? [self.comparison rows].size() : 0;
    for (int side = 0; side < 2; side++) {
        NSScrollView *scroll = side == 0 ? _leftScroll : _rightScroll;
        NSTextView *tv = scroll.documentView;
        const MCPane &pane = self.comparison ? [self.comparison pane:side == 0] : empty;
        const CGFloat textW = ceil((CGFloat)pane.columns * advance) + 8;
        const CGFloat textH = (CGFloat)MAX(rows, (size_t)1) * MCCompareRowHeight();
        CGFloat w = MAX(NSWidth(scroll.contentView.bounds), textW + kGutter + 24);
        CGFloat h = MAX(NSHeight(scroll.contentView.bounds), textH + 2 * tv.textContainerInset.height);
        [tv setFrameSize:NSMakeSize(w, h)];
    }
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    NSColor *bg = [cfg background:Surface::Editor];
    NSColor *text = [cfg text:Surface::Editor];
    self.layer.backgroundColor = bg.CGColor;
    for (NSTextField *h in @[self.leftHeader, self.rightHeader])
        h.textColor = [text colorWithAlphaComponent:0.75];
    self.changesLabel.textColor = [text colorWithAlphaComponent:0.55];
    // The decision bar: the sidebar's color, tinted with the accent.
    self.decisionBar.layer.backgroundColor =
        [[cfg background:Surface::Sidebar] blendedColorWithFraction:0.12
                                                            ofColor:CHex(0x4EA1F7, 1)].CGColor;
    self.decisionLabel.textColor = [cfg text:Surface::Sidebar];
    for (MCCompareTextView *tv in @[_left, _right]) {
        tv.numberColor = [text colorWithAlphaComponent:0.4];
        tv.textColor = text;
        tv.insertionPointColor = text;
        tv.needsDisplay = YES;
    }
    self.hunkGutter.needsDisplay = YES;
    if (self.comparison) [self fillPanes];
}

- (void)setComparison:(MCComparison *)comparison {
    if (_comparison != comparison) {
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        if (_comparison) [nc removeObserver:self name:MCComparisonDecisionChanged object:_comparison];
        if (comparison)
            [nc addObserver:self selector:@selector(decisionChanged:)
                       name:MCComparisonDecisionChanged object:comparison];
    }
    _comparison = comparison;
    _currentHunk = -1;
    self.leftHeader.stringValue = comparison.leftLabel ?: @"";
    self.rightHeader.stringValue = comparison.rightLabel ?: @"";
    self.leftHeader.toolTip = comparison.leftLabel;
    self.rightHeader.toolTip = comparison.rightLabel;
    [self fillPanes];
    [self showDecision];
    [self showChangesCount];
    self.scrollOrigin = NSZeroPoint;
}

// One pane's text with its syntax colors and changed words.
- (NSAttributedString *)paneString:(const MCPane &)pane left:(BOOL)left {
    AppSettings *cfg = [AppSettings shared];
    NSMutableParagraphStyle *rows = [NSMutableParagraphStyle new];
    rows.minimumLineHeight = rows.maximumLineHeight = MCCompareRowHeight();
    NSDictionary *attrs = @{
        NSFontAttributeName: MCCompareFont(),
        NSForegroundColorAttributeName: [cfg text:Surface::Editor],
        NSParagraphStyleAttributeName: rows};
    NSMutableAttributedString *s =
        [[NSMutableAttributedString alloc] initWithString:NSFromU16(pane.text) attributes:attrs];
    const NSUInteger len = s.length;
    [s beginEditing];
    // In order, so each run splits only the last one.
    NSColor *colors[8];
    for (int i = 0; i < 8; i++) colors[i] = [cfg syntax:(TokenStyle)i];
    for (const SideBySide::Span &sp : pane.syntax) {
        if (sp.start + sp.length > len || sp.style <= 0 || sp.style >= 8) continue;
        [s addAttribute:NSForegroundColorAttributeName value:colors[sp.style]
                  range:NSMakeRange(sp.start, sp.length)];
    }
    // VS Code's diff editor marks changed characters about twice as
    // strongly as their line.
    NSColor *word = left ? CHex(0xF14C4C, 0.38) : CHex(0x73C991, 0.36);
    for (const SideBySide::Span &sp : pane.words) {
        if (sp.start + sp.length > len) continue;
        [s addAttribute:NSBackgroundColorAttributeName value:word
                  range:NSMakeRange(sp.start, sp.length)];
    }
    [s endEditing];
    return s;
}

// Both panes from the rows: a side's line, or an empty filler row.
- (void)fillPanes {
    MCComparison *c = self.comparison;
    static const std::vector<SideBySide::Row> none;
    static const MCPane empty;
    const std::vector<SideBySide::Row> &rows = c ? [c rows] : none;
    const MCPane &lp = c ? [c pane:YES] : empty, &rp = c ? [c pane:NO] : empty;
    [_left.textStorage setAttributedString:[self paneString:lp left:YES]];
    [_right.textStorage setAttributedString:[self paneString:rp left:NO]];
    [_left setRows:rows starts:lp.starts];
    [_right setRows:rows starts:rp.starts];
    [self applyRowChoices];
    [self fitTextViews];
}

- (void)showChangesCount {
    const NSUInteger n = self.comparison.hunkCount;
    NSString *text = n == 0 ? (self.comparison ? @"No changes" : @"")
                   : _currentHunk >= 0 ? [NSString stringWithFormat:@"Change %ld of %lu",
                                                       (long)_currentHunk + 1, (unsigned long)n]
                   : n == 1 ? @"1 change"
                            : [NSString stringWithFormat:@"%lu changes", (unsigned long)n];
    self.changesLabel.stringValue = text;
}

- (NSPoint)scrollOrigin { return _leftScroll.contentView.bounds.origin; }
- (void)setScrollOrigin:(NSPoint)p {
    [self fitTextViews];
    [_leftScroll.contentView scrollToPoint:p];
    [_leftScroll reflectScrolledClipView:_leftScroll.contentView];
}

// One pane moved: the other follows, row for row.
- (void)paneScrolled:(NSNotification *)note {
    self.hunkGutter.needsDisplay = YES;
    if (_syncing) return;
    NSClipView *from = note.object;
    NSScrollView *other = from == _leftScroll.contentView ? _rightScroll : _leftScroll;
    _syncing = YES;
    NSRect want = other.contentView.bounds;
    want.origin = from.bounds.origin;
    NSPoint p = [other.contentView constrainBoundsRect:want].origin;
    [other.contentView scrollToPoint:p];
    [other reflectScrolledClipView:other.contentView];
    _syncing = NO;
}

// ---------------------------------------------------- stepping (F7)
- (NSUInteger)topRow {
    const size_t rows = self.comparison ? [self.comparison rows].size() : 0;
    if (rows == 0) return 0;
    // The row whose line holds the top edge, or the next when only a sliver
    // of it shows.
    const CGFloat y = NSMinY(_leftScroll.contentView.bounds) - _left.textContainerOrigin.y + 2;
    const long row = (long)floor(y / MCCompareRowHeight());
    return (NSUInteger)MAX(0L, MIN(row, (long)rows - 1));
}

// Whether any row of hunk `i` is on screen.
- (BOOL)hunkVisible:(NSInteger)i {
    const std::vector<SideBySide::Hunk> &hs = [self.comparison hunks];
    if (i < 0 || (size_t)i >= hs.size()) return NO;
    const NSRect first = [_left rectOfRow:hs[(size_t)i].first];
    const NSRect last = [_left rectOfRow:hs[(size_t)i].end - 1];
    return NSIntersectsRect(NSUnionRect(first, last), _leftScroll.contentView.bounds);
}

- (void)stepDifference:(BOOL)forward {
    MCComparison *c = self.comparison;
    if (!c || c.hunkCount == 0) {
        NSBeep();
        return;
    }
    const std::vector<SideBySide::Hunk> &hs = [c hunks];
    long from;
    if ([self hunkVisible:_currentHunk]) {
        from = (long)hs[(size_t)_currentHunk].first;
    } else {
        const long top = (long)self.topRow;
        from = forward ? top - 1 : top;
    }
    const int next = SideBySide::stepHunk(hs, from, forward);
    if (next < 0) return;
    _currentHunk = next;
    [self revealHunk:(size_t)next];
    [self showChangesCount];
}

// Scrolls hunk `i` a quarter of the way down the panes (or as near as the
// ends allow) and puts the caret at its start in the pane with the
// keyboard, or the right one, as VS Code puts the cursor on the change.
- (void)revealHunk:(size_t)i {
    const SideBySide::Hunk h = [self.comparison hunks][i];
    [self fitTextViews];
    const NSRect row = [_left rectOfRow:h.first];
    NSClipView *clip = _leftScroll.contentView;
    NSRect want = clip.bounds;
    want.origin.y = NSMinY(row) - floor(NSHeight(want) / 4);
    want.origin.y = MAX(0, MIN(want.origin.y, NSHeight(_left.frame) - NSHeight(want)));
    want.origin.x = 0;
    [clip scrollToPoint:[clip constrainBoundsRect:want].origin];
    [_leftScroll reflectScrolledClipView:clip];
    NSResponder *fr = self.window.firstResponder;
    MCCompareTextView *pane = fr == _left ? _left : _right;
    [pane setSelectedRange:NSMakeRange([pane startOfRow:h.first], 0)];
    self.hunkGutter.needsDisplay = YES;
}

- (IBAction)goToNextDifference:(id)sender {
    (void)sender;
    [self stepDifference:YES];
}

- (IBAction)goToPreviousDifference:(id)sender {
    (void)sender;
    [self stepDifference:NO];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(goToNextDifference:) ||
        item.action == @selector(goToPreviousDifference:))
        return self.comparison.hunkCount > 0 && !self.hidden;
    return YES;
}

// ------------------------------------------------- the per-hunk buttons
- (NSRect)rectForHunk:(NSUInteger)hunk accept:(BOOL)accept {
    MCComparison *c = self.comparison;
    if (self.hunkGutter.hidden || hunk >= c.hunkCount) return NSZeroRect;
    const SideBySide::Hunk h = [c hunks][hunk];
    NSRect row = [_left rectOfRow:h.first];
    // Into this view: the pane's top, less how far it is scrolled.
    const CGFloat y = NSMinY(_leftScroll.frame) + NSMinY(row) - NSMinY(_leftScroll.contentView.bounds);
    const NSRect pane = _leftScroll.frame;
    const CGFloat mid = y + NSHeight(row) / 2;
    if (mid < NSMinY(pane) || mid > NSMaxY(pane)) return NSZeroRect;
    const NSRect g = self.hunkGutter.frame;
    const CGFloat x = NSMinX(g) + floor((NSWidth(g) - 2 * kHunkButton - 4) / 2) + (accept ? 0 : kHunkButton + 4);
    return NSMakeRect(x, floor(mid - kHunkButton / 2), kHunkButton, kHunkButton);
}

- (BOOL)pressHunkButtonAtPoint:(NSPoint)point {
    MCComparison *c = self.comparison;
    if (self.hunkGutter.hidden) return NO;
    for (NSUInteger i = 0; i < c.hunkCount; i++) {
        const NSInteger d = [c decisionOfHunk:i];
        for (int accept = 1; accept >= 0; accept--) {
            if (!NSPointInRect(point, [self rectForHunk:i accept:accept])) continue;
            // An undecided hunk takes the choice; a decided one shows only its
            // choice, and a press on it makes the hunk undecided again.
            if (d == 0) [c decideHunk:i as:accept ? 1 : -1];
            else if ((d > 0) == (accept == 1)) [c decideHunk:i as:0];
            else continue;
            return YES;
        }
    }
    return NO;
}

- (void)drawHunkButtonsIn:(NSView *)gutter {
    MCComparison *c = self.comparison;
    AppSettings *cfg = [AppSettings shared];
    [[[cfg background:Surface::Editor] blendedColorWithFraction:0.06 ofColor:NSColor.grayColor] set];
    NSRectFill(gutter.bounds);
    [CHex(0x333333, 1) set];
    NSRectFill(NSMakeRect(0, 0, 1, NSHeight(gutter.bounds)));
    NSRectFill(NSMakeRect(NSWidth(gutter.bounds) - 1, 0, 1, NSHeight(gutter.bounds)));
    [gutter removeAllToolTips];
    NSDictionary *glyph = @{
        NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightBold],
        NSForegroundColorAttributeName: NSColor.whiteColor};
    for (NSUInteger i = 0; i < c.hunkCount; i++) {
        const NSInteger d = [c decisionOfHunk:i];
        for (int accept = 1; accept >= 0; accept--) {
            if (d != 0 && (d > 0) != (accept == 1)) continue;
            const NSRect r = [self rectForHunk:i accept:accept];
            if (NSIsEmptyRect(r)) continue;
            const NSRect local = [gutter convertRect:r fromView:self];
            NSColor *fill = accept ? CHex(0x2E7D4F, d ? 1.0 : 0.55) : CHex(0xB3363A, d ? 1.0 : 0.55);
            [fill set];
            [[NSBezierPath bezierPathWithRoundedRect:local xRadius:3 yRadius:3] fill];
            NSString *mark = accept ? @"✓" : @"✕";
            const NSSize sz = [mark sizeWithAttributes:glyph];
            [mark drawAtPoint:NSMakePoint(NSMidX(local) - sz.width / 2, NSMidY(local) - sz.height / 2)
               withAttributes:glyph];
            NSString *tip = d ? (accept ? @"Accepted; click to decide again"
                                        : @"Rejected; click to decide again")
                              : (accept ? @"Accept this change" : @"Reject this change");
            [gutter addToolTipRect:local owner:tip userData:NULL];
        }
    }
}
@end
