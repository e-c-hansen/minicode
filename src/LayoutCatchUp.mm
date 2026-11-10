// LayoutCatchUp.mm — see LayoutCatchUp.h.
#import "LayoutCatchUp.h"
#include <algorithm>
#include <time.h>
#include <vector>

namespace {

double ThreadCpuMs() { return (double)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e6; }

// About this much of the main thread a run-loop turn, so typing, scrolling
// and clicks during the catch-up wait at most that long.
const double kPieceBudgetMs = 8;
// A turn lays out chunks of about half the budget, each as large as text
// not laid out at all costs in that time, until it has spent three
// quarters of the budget: one more chunk of fresh text then ends it near
// the budget, and stretches already laid out go by many to a turn.
const double kChunkMs = kPieceBudgetMs / 2, kTurnMs = kPieceBudgetMs * 0.75;
const NSUInteger kMinPiece = 1024, kMaxPiece = 1 << 20;
// Before anything is measured: a 4 KB chunk.
const double kFirstCharsPerMs = 4096 / kChunkMs;
// After an edit or a new width, how long to wait before laying out again,
// so typing is not followed by a piece per keystroke.
const NSTimeInterval kAfterChange = 0.3;

}  // namespace

NSNotificationName const MCTextViewWillReplaceManyRangesNotification =
    @"MCTextViewWillReplaceManyRangesNotification";

@implementation MCLayoutCatchUp {
    __weak NSTextView *_tv;
    __weak NSScrollView *_scroll;
    __weak NSTextStorage *_observedStorage;
    // How many characters of text not laid out at all a millisecond lays
    // out (see -measure:characters:).
    double _charsPerMs;
    std::vector<double> _turns;   // each turn's CPU, for the checks
    NSUInteger _below;   // where the next piece ends: laid out from here down to the pane
    // Nothing was unlaid above the pane when last looked, and the width is
    // the same since: a gap there now is the latest edit's.
    BOOL _settled;
    CGFloat _width;
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
    _charsPerMs = kFirstCharsPerMs;
    _turns.clear();
    _below = NSNotFound;
    _settled = NO;
    _width = tv.textContainer.size.width;
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
    [nc addObserver:self selector:@selector(manyRangesComing:)
               name:MCTextViewWillReplaceManyRangesNotification object:tv];
    [self catchUp];
}

// See MCTextViewWillReplaceManyRangesNotification. Turning non-contiguous
// layout off throws the layout away, so the pane is laid out from the top
// when next drawn, the cost the catch-up spared when the tab showed, paid
// once with an edit that costs as much again anyway.
- (void)manyRangesComing:(NSNotification *)note {
    (void)note;
    NSTextView *tv = _tv;
    if (!_watching || !tv.layoutManager.allowsNonContiguousLayout) return;
    [self cancel];
    tv.layoutManager.allowsNonContiguousLayout = NO;
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
        [nc removeObserver:self name:MCTextViewWillReplaceManyRangesNotification object:tv];
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
    // A gap that was there before the edit (the first catch-up, a new
    // width) is not the edit's: the pieces close it, and a keystroke never
    // waits for it.
    if (!_watching || !_settled || !tc || !clip || !lm.allowsNonContiguousLayout ||
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
    // Glyphs that only moved cost little, so the passes double; none is
    // larger than fresh text could lay out in the time left.
    NSUInteger piece = [self chunkFor:kChunkMs];
    for (double spent = 0; first < top && spent < 30; spent = ThreadCpuMs() - c0) {
        const NSUInteger n = MIN(MIN(piece, [self chunkFor:30 - spent]), top - first);
        const double p0 = ThreadCpuMs();
        [lm ensureLayoutForCharacterRange:NSMakeRange(first, n)];
        [self measure:ThreadCpuMs() - p0 characters:n];
        [lm getFirstUnlaidCharacterIndex:&first glyphIndex:&firstGlyph];
        piece = MIN(piece * 2, kMaxPiece);
    }
    if (first >= top && !NSEqualPoints(clip.bounds.origin, place)) {
        [clip scrollToPoint:place];
        [_scroll reflectScrolledClipView:clip];
    }
    if (first < top) _settled = NO;   // the rest is the pieces' (storageEdited asked for them)
    [self noteTurn:ThreadCpuMs() - c0];
}

// A new width (a resize, word wrap, the zoom) lays out again; so may folds.
- (void)frameChanged:(NSNotification *)note {
    (void)note;
    const CGFloat width = _tv.textContainer.size.width;
    if (width != _width) {
        _width = width;
        _settled = NO;
    }
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

// The pace of fresh text, from a stretch just laid out. A stretch partly
// laid out already costs less for its size, and sizing the next from it
// grew the pieces over a run of such stretches (screens passed while
// dragging the scroller, say) to a megabyte, which the first fresh text
// after them laid out in one turn: 149 ms in the keystroke suite. So a
// stretch counts only when it cost at least half of what fresh text of its
// size costs at the pace known (at a quarter, a stretch partly laid out
// made the pace 2.8 times the real one and a chunk 12 ms), and the pace
// moves at most 1.5 times up and half down a stretch: up, so a chunk of
// fresh text costs at most the budget; down, so one stretch slowed by
// the machine leaves the pace within reach of the next one's count.
- (void)measure:(double)ms characters:(NSUInteger)n {
    if (ms < 0.5 || n == 0) return;   // too short to time
    const double expected = n / _charsPerMs;
    if (ms < expected / 2) return;
    _charsPerMs = std::clamp(n / ms, _charsPerMs / 2, _charsPerMs * 1.5);
}

- (NSUInteger)chunkFor:(double)ms {
    return (NSUInteger)std::clamp(_charsPerMs * ms, (double)kMinPiece, (double)kMaxPiece);
}

- (void)noteTurn:(double)ms {
    _pieces++;
    _totalMilliseconds += ms;
    _longestPieceMilliseconds = MAX(_longestPieceMilliseconds, ms);
    _turns.push_back(ms);
}

- (double)turnMillisecondsAtFraction:(double)fraction {
    if (_turns.empty()) return 0;
    std::vector<double> t = _turns;
    const size_t k = std::min(t.size() - 1, (size_t)(fraction * (t.size() - 1) + 0.5));
    std::nth_element(t.begin(), t.begin() + (long)k, t.end());
    return t[k];
}

// One piece: the stretch just above what is laid out down to the pane,
// upward from the pane, until nothing above the pane's top is unlaid (the
// lines nearest the pane are exact first, and nothing below it is waited
// for). Turns of about 8 ms, not less: after each hole it fills TextKit
// looks up the attributes at the pane and the caret (the visible range,
// the blink rect), and with the highlighter's hundreds of thousands of
// color runs a lookup far from the last one walks the runs between, a
// cost per piece. Measured at the middle of 100,000 lines (keystroke
// suite): 1.8 s of CPU in idle turns against the 1.2 s contiguous layout
// spent before the tab could show, on a quiet Mac, and 6 to 7.5 s against
// 2.3 on a loaded one; pieces of 2,048
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
        _settled = _width == tv.textContainer.size.width;
        return;
    }
    _running = YES;
    _settled = NO;
    const double c0 = ThreadCpuMs();
    NSString *text = tv.textStorage.string;
    if (_below == NSNotFound || _below > at) _below = at;
    // Chunks until three quarters of the budget is spent (see kChunkMs).
    for (int chunks = 0; chunks < 1024 && _below > first && ThreadCpuMs() - c0 < kTurnMs; chunks++) {
        const NSUInteger chunk = [self chunkFor:kChunkMs];
        NSUInteger from = _below > chunk ? _below - chunk : 0;
        from = MAX(first, [text lineRangeForRange:NSMakeRange(from, 0)].location);
        const double p0 = ThreadCpuMs();
        [lm ensureLayoutForCharacterRange:NSMakeRange(from, _below - from)];
        [self measure:ThreadCpuMs() - p0 characters:_below - from];
        _below = from;
    }
    // What is on screen stays put while the estimate above it is replaced
    // by the real height. NSTextView does that itself with non-contiguous
    // layout (measured: the pane's top line never moved); this makes sure.
    [self keepAnchor:at offset:offset];
    // The top hole is closed; another may be left between it and the pane
    // (a jump far down and back): from the pane upward again.
    if (_below <= first) _below = NSNotFound;
    [self noteTurn:ThreadCpuMs() - c0];
    [self performSelector:@selector(step) withObject:nil afterDelay:0
                  inModes:@[NSDefaultRunLoopMode]];
}

@end
