// LayoutCatchUp.mm — see LayoutCatchUp.h.
#import "LayoutCatchUp.h"
#include <time.h>

namespace {

double ThreadCpuMs() { return (double)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e6; }

// About this much of the main thread a run-loop turn, so typing, scrolling
// and clicks during the catch-up wait at most that long.
const double kPieceBudgetMs = 8;
const NSUInteger kFirstPiece = 4 * 1024, kMinPiece = 1024, kMaxPiece = 1 << 20;
// After an edit or a new width, how long to wait before laying out again,
// so typing is not followed by a piece per keystroke.
const NSTimeInterval kAfterChange = 0.3;

}  // namespace

@implementation MCLayoutCatchUp {
    __weak NSTextView *_tv;
    __weak NSScrollView *_scroll;
    __weak NSTextStorage *_observedStorage;
    NSUInteger _piece;
    NSUInteger _below;   // where the next piece ends: laid out from here down to the pane
}

- (instancetype)initWithTextView:(NSTextView *)textView scrollView:(NSScrollView *)scroll {
    if ((self = [super init])) {
        _tv = textView;
        _scroll = scroll;
    }
    return self;
}

- (void)dealloc {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)prepare {
    [self cancel];
    _tv.layoutManager.allowsNonContiguousLayout = YES;
}

- (void)start {
    NSTextView *tv = _tv;
    if (!tv.layoutManager.allowsNonContiguousLayout) return;
    _watching = YES;
    _piece = kFirstPiece;
    _below = NSNotFound;
    _longestPieceMilliseconds = 0;
    _totalMilliseconds = 0;
    _pieces = 0;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    _observedStorage = tv.textStorage;
    [nc addObserver:self selector:@selector(storageEdited:)
               name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
    tv.postsFrameChangedNotifications = YES;
    [nc addObserver:self selector:@selector(frameChanged:)
               name:NSViewFrameDidChangeNotification object:tv];
    [nc addObserver:self selector:@selector(textChanged:) name:NSTextDidChangeNotification object:tv];
    [self catchUp];
}

- (void)cancel {
    _running = NO;
    _watching = NO;
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (NSTextStorage *ts = _observedStorage)
        [nc removeObserver:self name:NSTextStorageDidProcessEditingNotification object:ts];
    if (NSTextView *tv = _tv) {
        [nc removeObserver:self name:NSViewFrameDidChangeNotification object:tv];
        [nc removeObserver:self name:NSTextDidChangeNotification object:tv];
    }
    _observedStorage = nil;
}

// The default mode only: while the user drags a scroller or selects text
// (the tracking mode) the place is not moved under the pointer.
- (void)catchUp {
    _running = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(step) object:nil];
    [self performSelector:@selector(step) withObject:nil afterDelay:0
                  inModes:@[NSDefaultRunLoopMode]];
}

- (void)later {
    if (!_watching || _running) return;   // a running catch-up gets there anyway
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(step) object:nil];
    [self performSelector:@selector(step) withObject:nil afterDelay:kAfterChange
                  inModes:@[NSDefaultRunLoopMode]];
}

- (void)storageEdited:(NSNotification *)note {
    if (!(((NSTextStorage *)note.object).editedMask & NSTextStorageEditedCharacters)) return;
    _below = NSNotFound;   // its characters moved
    [self later];
}

// An edit made through the text view (typing, a paste, the editor's
// programmatic edits, undo), once it is done: what it left unlaid above the
// pane is laid out at once, before anything reads a rect or the pane is
// drawn, and the pane keeps its place in the text view's coordinates, so
// the edit moves the text below it exactly as contiguous layout does (a
// new line above the pane moves the pane's text down a line). Left alone,
// non-contiguous layout moved it by an estimate (4 points up, for a new
// line of 18 above it), and NSTextView, which with non-contiguous layout
// scrolls to keep the pane's text still as the layout above it changes,
// kept it there. An
// edit in the pane, which typing is, leaves nothing unlaid above it and
// costs two lookups. What is laid out is what contiguous layout lays out to
// draw the pane, so it costs no more: mostly glyphs that only moved, a few
// milliseconds; past 30 ms the rest goes to the pieces, keeping the pane
// still.
- (void)textChanged:(NSNotification *)note {
    (void)note;
    NSTextView *tv = _tv;
    NSClipView *clip = _scroll.contentView;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    // While the first catch-up runs the gap above the pane is its own, not
    // the edit's: the pieces close it.
    if (!_watching || _running || !tc || !clip || !lm.allowsNonContiguousLayout ||
        (_stillWanted && !_stillWanted()))
        return;
    NSUInteger first = 0, firstGlyph = 0;
    [lm getFirstUnlaidCharacterIndex:&first glyphIndex:&firstGlyph];
    const NSPoint o = tv.textContainerOrigin;
    const NSRect pane = NSOffsetRect(tv.visibleRect, -o.x, -o.y);
    const NSRange glyphs = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:pane inTextContainer:tc];
    if (glyphs.length == 0) return;
    const NSUInteger top = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL].location;
    if (first >= top) return;
    const double c0 = ThreadCpuMs();
    const NSPoint place = clip.bounds.origin;
    NSUInteger piece = kFirstPiece;
    while (first < top && ThreadCpuMs() - c0 < 30) {
        [lm ensureLayoutForCharacterRange:NSMakeRange(first, MIN(piece, top - first))];
        [lm getFirstUnlaidCharacterIndex:&first glyphIndex:&firstGlyph];
        piece = MIN(piece * 2, (NSUInteger)kMaxPiece);
    }
    if (first >= top && !NSEqualPoints(clip.bounds.origin, place)) {
        [clip scrollToPoint:place];
        [_scroll reflectScrolledClipView:clip];
    }
    const double spent = ThreadCpuMs() - c0;
    _pieces++;
    _totalMilliseconds += spent;
    _longestPieceMilliseconds = MAX(_longestPieceMilliseconds, spent);
}

// A new width (a resize, word wrap, the zoom) lays out again; so may folds.
- (void)frameChanged:(NSNotification *)note {
    (void)note;
    [self later];
}

// The character at the top of the pane and how far its line fragment is
// from the pane's top, read from what is laid out there.
- (BOOL)anchor:(NSUInteger *)character offset:(CGFloat *)offset {
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    if (!tc || tv.textStorage.length == 0) return NO;
    const NSPoint o = tv.textContainerOrigin;
    const NSRect visible = tv.visibleRect;
    const NSPoint p = NSMakePoint(MAX(0, NSMinX(visible) - o.x), MAX(0, NSMinY(visible) - o.y));
    // (Never numberOfGlyphs: it generates the glyphs of the whole text.)
    const NSUInteger g = [lm glyphIndexForPoint:p inTextContainer:tc];
    const NSUInteger c = [lm characterIndexForGlyphAtIndex:g];
    if (c >= tv.textStorage.length) return NO;
    const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
    *character = c;
    *offset = NSMinY(frag) + o.y - NSMinY(visible);
    return YES;
}

- (void)keepAnchor:(NSUInteger)character offset:(CGFloat)offset {
    NSTextView *tv = _tv;
    NSScrollView *scroll = _scroll;
    NSLayoutManager *lm = tv.layoutManager;
    if (!scroll || character >= tv.textStorage.length) return;
    const NSUInteger g = [lm glyphIndexForCharacterAtIndex:character];
    const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
    NSClipView *clip = scroll.contentView;
    // The clip view's bounds are in the text view's coordinates (the text
    // view is the document view, at the origin).
    const CGFloat y = NSMinY(frag) + tv.textContainerOrigin.y - offset;
    if (fabs(y - NSMinY(clip.bounds)) < 0.5) return;
    [clip scrollToPoint:NSMakePoint(NSMinX(clip.bounds), MAX(0, y))];
    [scroll reflectScrolledClipView:clip];
}

// One piece: the stretch just above what is laid out down to the pane,
// upward from the pane, until nothing above the pane's top is unlaid (the
// lines nearest the pane are exact first, and nothing below it is waited
// for). Pieces of about 8 ms, not less: after each hole it fills TextKit
// looks up the attributes at the pane and the caret (the visible range,
// the blink rect), and with the highlighter's hundreds of thousands of
// color runs a lookup far from the last one walks the runs between, a
// cost per piece. Measured at the middle of 100,000 lines: 6 to 7.5 s of
// CPU in idle turns, against 2.3 s that contiguous layout spent before the
// tab could show (keystroke suite, the same Mac), and pieces of 2,048
// characters cost two and a half times as much in all as pieces of 65,536.
- (void)step {
    if (!_watching) return;
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (!tv || !lm.allowsNonContiguousLayout || (_stillWanted && !_stillWanted())) {
        [self cancel];
        return;
    }
    NSUInteger at = 0;
    CGFloat offset = 0;
    NSUInteger first = 0, firstGlyph = 0;
    [lm getFirstUnlaidCharacterIndex:&first glyphIndex:&firstGlyph];
    if (![self anchor:&at offset:&offset] || first >= at) {
        _running = NO;   // laid out from the top through the pane: those rects exact
        _below = NSNotFound;
        return;
    }
    _running = YES;
    const double c0 = ThreadCpuMs();
    NSString *text = tv.textStorage.string;
    if (_below == NSNotFound || _below > at) _below = at;
    // Stretches already laid out cost next to nothing: several go in one
    // turn, until a piece does real work.
    double spent = 0;
    for (int tries = 0; tries < 64 && _below > first; tries++) {
        NSUInteger from = _below > _piece ? _below - _piece : 0;
        from = MAX(first, [text lineRangeForRange:NSMakeRange(from, 0)].location);
        const double p0 = ThreadCpuMs();
        [lm ensureLayoutForCharacterRange:NSMakeRange(from, _below - from)];
        _below = from;
        const double piece = ThreadCpuMs() - p0;
        spent += piece;
        if (piece > 0.5) {
            // The next piece sized to the budget from what this one cost.
            _piece = (NSUInteger)MIN((double)kMaxPiece,
                                     MAX((double)kMinPiece, _piece * MIN(2.0, kPieceBudgetMs / piece)));
            break;
        }
    }
    // What is on screen stays put while the estimate above it is replaced
    // by the real height. NSTextView does that itself with non-contiguous
    // layout (measured: the pane's top line never moved); this makes sure.
    [self keepAnchor:at offset:offset];
    // The top hole is closed; another may be left between it and the pane
    // (a jump far down and back): from the pane upward again.
    if (_below <= first) _below = NSNotFound;
    spent = ThreadCpuMs() - c0;
    _pieces++;
    _totalMilliseconds += spent;
    _longestPieceMilliseconds = MAX(_longestPieceMilliseconds, spent);
    [self performSelector:@selector(step) withObject:nil afterDelay:0
                  inModes:@[NSDefaultRunLoopMode]];
}

@end
