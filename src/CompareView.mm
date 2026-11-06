// CompareView.mm — see CompareView.h.
#import "CompareView.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Appearance.h"
#include "SideBySide.h"
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

// The lines of a text, without their line breaks.
static NSArray<NSString *> *MCLines(NSString *text) {
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                             options:NSStringEnumerationByLines
                          usingBlock:^(NSString *line, NSRange, NSRange, BOOL *) {
        [lines addObject:line ?: @""];
    }];
    return lines;
}

// ---------------------------------------------------------------- the model
// Posted (object: the comparison) when its decision changes, so the view
// showing it redraws its bar.
static NSNotificationName const MCComparisonDecisionChanged = @"MCComparisonDecisionChanged";

@interface MCComparison ()
- (const std::vector<SideBySide::Row> &)rows;
@end

@implementation MCComparison {
    std::vector<SideBySide::Row> _rows;
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
// The alignment, once: it is the costly part of a big comparison, and both
// the count and the view need it.
- (void)prepare {
    if (_prepared) return;
    _rows = SideBySide::align(U16(_leftText ?: @""), U16(_rightText ?: @""));
    _changed = 0;
    for (const SideBySide::Row &r : _rows)
        if (r.kind != SideBySide::Kind::Same) _changed++;
    _prepared = YES;
}
- (const std::vector<SideBySide::Row> &)rows {
    [self prepare];
    return _rows;
}
- (NSUInteger)changedRows {
    [self prepare];
    return _changed;
}
- (void)decide:(BOOL)accepted {
    if (!_decisionPrompt || _decision != 0) return;
    _decision = accepted ? 1 : -1;
    void (^done)(BOOL) = _onDecision;
    _onDecision = nil;
    [[NSNotificationCenter defaultCenter] postNotificationName:MCComparisonDecisionChanged
                                                        object:self];
    if (done) done(accepted);
}
- (void)endDecision:(NSString *)why {
    if (!_decisionPrompt || _decision != 0) return;
    _decision = 2;
    _decisionNote = [why copy];
    _onDecision = nil;
    [[NSNotificationCenter defaultCenter] postNotificationName:MCComparisonDecisionChanged
                                                        object:self];
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
- (void)setRows:(const std::vector<SideBySide::Row> &)rows starts:(std::vector<NSUInteger>)starts;
@end

static const CGFloat kGutter = 52;

@implementation MCCompareTextView {
    std::vector<SideBySide::Row> _rows;
    std::vector<NSUInteger> _starts;   // each row's first character
}

- (void)setRows:(const std::vector<SideBySide::Row> &)rows starts:(std::vector<NSUInteger>)starts {
    _rows = rows;
    _starts = std::move(starts);
    self.needsDisplay = YES;
}

- (size_t)rowAtCharacter:(NSUInteger)index {
    auto it = std::upper_bound(_starts.begin(), _starts.end(), index);
    return it == _starts.begin() ? 0 : (size_t)(it - _starts.begin() - 1);
}

- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    NSLayoutManager *lm = self.layoutManager;
    if (!lm || _rows.empty()) return;
    const NSPoint origin = self.textContainerOrigin;
    NSRange glyphs = [lm glyphRangeForBoundingRect:NSOffsetRect(rect, -origin.x, -origin.y)
                                   inTextContainer:self.textContainer];
    NSDictionary *numAttrs = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: self.numberColor ?: [NSColor grayColor]};
    NSColor *removed = CHex(0xF14C4C, 0.16), *added = CHex(0x73C991, 0.16);
    NSColor *filler = [NSColor colorWithWhite:0.5 alpha:0.07];
    const CGFloat W = NSWidth(self.bounds);
    auto drawRow = [&](size_t r, NSRect frag) {
        NSRect line = NSMakeRect(0, frag.origin.y + origin.y, W, frag.size.height);
        if (!NSIntersectsRect(line, rect)) return;
        const SideBySide::Row &row = _rows[r];
        const int number = self.leftSide ? row.left : row.right;
        NSColor *tint = nil;
        if (number < 0) tint = filler;
        else if (row.kind == SideBySide::Kind::Removed || row.kind == SideBySide::Kind::Added ||
                 row.kind == SideBySide::Kind::Changed)
            tint = self.leftSide ? removed : added;
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
    [lm enumerateLineFragmentsForGlyphRange:glyphs
                                 usingBlock:^(NSRect frag, NSRect, NSTextContainer *, NSRange g, BOOL *) {
        NSUInteger ch = [lm characterIndexForGlyphAtIndex:g.location];
        drawRow([self rowAtCharacter:ch], frag);
    }];
    // The last row, when it is empty, has no line fragment of its own.
    NSRect extra = lm.extraLineFragmentRect;
    if (!NSIsEmptyRect(extra) && _rows.size() == _starts.size()) drawRow(_rows.size() - 1, extra);
}
@end

// ---------------------------------------------------------- the two panes
@interface MCCompareView ()
@property(nonatomic, strong) NSTextField *leftHeader, *rightHeader;
@property(nonatomic, strong) NSView *divider;
// The bar over the panes while a change waits for a decision.
@property(nonatomic, strong) NSView *decisionBar;
@property(nonatomic, strong) NSTextField *decisionLabel;
@end

@implementation MCCompareView {
    MCCompareTextView *_left, *_right;
    NSScrollView *_leftScroll, *_rightScroll;
    BOOL _syncing;
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
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = YES;   // lines never wrap, so rows stay level
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
        _leftScroll = [self makePane:&_left left:YES];
        _rightScroll = [self makePane:&_right left:NO];
        [self addSubview:_leftScroll];
        [self addSubview:_rightScroll];
        self.leftHeader = [self makeHeader];
        self.rightHeader = [self makeHeader];
        [self addSubview:self.leftHeader];
        [self addSubview:self.rightHeader];
        self.divider = [[NSView alloc] initWithFrame:NSZeroRect];
        self.divider.wantsLayer = YES;
        self.divider.layer.backgroundColor = CHex(0x333333, 1).CGColor;
        [self addSubview:self.divider];
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

- (void)acceptChange:(id)sender { (void)sender; [self.comparison decide:YES]; }
- (void)rejectChange:(id)sender { (void)sender; [self.comparison decide:NO]; }

- (void)decisionChanged:(NSNotification *)note {
    if (note.object == self.comparison) [self showDecision];
}

// The bar's words and buttons for the comparison's decision.
- (void)showDecision {
    MCComparison *c = self.comparison;
    const BOOL wanted = c.decisionPrompt.length > 0;
    const BOOL changed = self.decisionBar.hidden == wanted;
    self.decisionBar.hidden = !wanted;
    if (wanted) {
        NSString *text = c.decisionPrompt;
        if (c.decision == 1) text = @"Accepted: Claude Code writes the change.";
        else if (c.decision == -1) text = @"Rejected.";
        else if (c.decision == 2) text = c.decisionNote ?: @"No longer waiting for an answer.";
        self.decisionLabel.stringValue = text;
        self.decisionLabel.toolTip = text;
        _acceptButton.hidden = _rejectButton.hidden = c.decision != 0;
    }
    if (changed) [self layoutPanes];
    else [self layoutDecisionBar];
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

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self layoutPanes];
}

- (void)layoutPanes {
    [self layoutDecisionBar];
    const NSRect b = self.bounds;
    const CGFloat top = NSHeight(self.decisionBar.frame);
    const CGFloat headerH = 24, half = floor(NSWidth(b) / 2);
    self.leftHeader.frame = NSMakeRect(10, top + 4, MAX(0, half - 20), 16);
    self.rightHeader.frame = NSMakeRect(half + 10, top + 4, MAX(0, NSWidth(b) - half - 20), 16);
    _leftScroll.frame = NSMakeRect(0, top + headerH, half, MAX(0, NSHeight(b) - top - headerH));
    _rightScroll.frame = NSMakeRect(half + 1, top + headerH, MAX(0, NSWidth(b) - half - 1),
                                    MAX(0, NSHeight(b) - top - headerH));
    self.divider.frame = NSMakeRect(half, top, 1, MAX(0, NSHeight(b) - top));
    [self fitTextViews];
}

// Each text view as wide as its longest line or its pane, whichever is more.
- (void)fitTextViews {
    for (NSScrollView *scroll in @[_leftScroll, _rightScroll]) {
        NSTextView *tv = scroll.documentView;
        [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        NSRect used = [tv.layoutManager usedRectForTextContainer:tv.textContainer];
        CGFloat w = MAX(NSWidth(scroll.contentView.bounds), NSWidth(used) + kGutter + 24);
        CGFloat h = MAX(NSHeight(scroll.contentView.bounds), NSHeight(used) + 12);
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
    [self showDecision];
    self.leftHeader.stringValue = comparison.leftLabel ?: @"";
    self.rightHeader.stringValue = comparison.rightLabel ?: @"";
    self.leftHeader.toolTip = comparison.leftLabel;
    self.rightHeader.toolTip = comparison.rightLabel;
    [self fillPanes];
    self.scrollOrigin = NSZeroPoint;
}

// Both panes from the rows: a side's line, or an empty filler row.
- (void)fillPanes {
    MCComparison *c = self.comparison;
    NSString *a = c.leftText ?: @"", *b = c.rightText ?: @"";
    static const std::vector<SideBySide::Row> none;
    const std::vector<SideBySide::Row> &rows = c ? [c rows] : none;
    NSArray<NSString *> *la = MCLines(a), *lb = MCLines(b);
    NSMutableString *left = [NSMutableString string], *right = [NSMutableString string];
    std::vector<NSUInteger> ls, rs;
    ls.reserve(rows.size());
    rs.reserve(rows.size());
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0) {
            [left appendString:@"\n"];
            [right appendString:@"\n"];
        }
        ls.push_back(left.length);
        rs.push_back(right.length);
        if (rows[r].left >= 0 && (NSUInteger)rows[r].left < la.count) [left appendString:la[(NSUInteger)rows[r].left]];
        if (rows[r].right >= 0 && (NSUInteger)rows[r].right < lb.count) [right appendString:lb[(NSUInteger)rows[r].right]];
    }
    AppSettings *cfg = [AppSettings shared];
    NSDictionary *attrs = @{
        NSFontAttributeName: MCMonoFont(12.5, NSFontWeightRegular),
        NSForegroundColorAttributeName: [cfg text:Surface::Editor]};
    [_left.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:left attributes:attrs]];
    [_right.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:right attributes:attrs]];
    [_left setRows:rows starts:ls];
    [_right setRows:rows starts:rs];
    [self fitTextViews];
}

- (NSPoint)scrollOrigin { return _leftScroll.contentView.bounds.origin; }
- (void)setScrollOrigin:(NSPoint)p {
    [self fitTextViews];
    [_leftScroll.contentView scrollToPoint:p];
    [_leftScroll reflectScrolledClipView:_leftScroll.contentView];
}

// One pane moved: the other follows, row for row.
- (void)paneScrolled:(NSNotification *)note {
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
@end
