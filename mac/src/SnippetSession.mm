// SnippetSession.mm — see SnippetSession.h.
#import "SnippetSession.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "SourceStorage.h"
#include <vector>

@implementation MCSnippetSession {
    __weak CodeTextView *_tv;
    Snippet::Session _s;
    BOOL _active;
    BOOL _applying;   // the session is setting the selection or writing copies
    NSRect _drawn;    // what the stops' boxes covered when last drawn, in the view
}

- (instancetype)initWithTextView:(CodeTextView *)tv {
    if ((self = [super init])) {
        _tv = tv;
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        // The editor's storage tells each replacement exactly; any other
        // only the union of an editing session's changes, at its end.
        if ([tv.textStorage isKindOfClass:[MCSourceStorage class]]) {
            __weak MCSnippetSession *weakSelf = self;
            ((MCSourceStorage *)tv.textStorage).onReplaceCharacters = ^(NSRange r, NSUInteger n) {
                [weakSelf replaced:r length:n];
            };
        } else {
            [nc addObserver:self selector:@selector(storageEdited:)
                       name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
        }
        [nc addObserver:self selector:@selector(selectionChanged:)
                   name:NSTextViewDidChangeSelectionNotification object:tv];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

- (BOOL)active { return _active; }

static NSRange RangeOf(const Snippet::Range &r) { return NSMakeRange(r.start, r.end - r.start); }

- (void)startWithExpansion:(const Snippet::Expansion &)expansion base:(NSUInteger)base {
    [self end];
    if (!expansion.hasStops()) return;
    _s = Snippet::Session(expansion, base);
    _active = YES;
    [self selectCurrent];
    [self invalidate];
}

- (void)end {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(checkSelection)
                                               object:nil];
    if (!_active) return;
    _active = NO;
    _s.end();
    [self invalidate];
}

// ---- keys

- (BOOL)doCommand:(SEL)sel {
    if (!_active) return NO;
    const BOOL tab = sel == @selector(insertTab:) || sel == @selector(insertTabIgnoringFieldEditor:);
    const BOOL backTab = sel == @selector(insertBacktab:);
    const BOOL esc = sel == @selector(cancelOperation:);
    if (!tab && !backTab && !esc) return NO;
    if (_commandTakenFirst && _commandTakenFirst(sel)) return YES;
    if (!_active) return NO;   // the list's key ended it
    if (esc) {
        [self end];
        return YES;
    }
    [self writeTransforms];
    if (!_active) return YES;
    if (backTab) {
        if (_s.prev()) [self selectCurrent];
        return YES;
    }
    if (!_s.next()) {
        // Nowhere left (the final stop was typed over): the snippet's end.
        const NSUInteger end = MIN(_s.span().end, _tv.string.length);
        [self end];
        [self selectRanges:{{end, end}}];
        return YES;
    }
    if (_s.atFinal()) {
        // $0 (or ${0:text}, selected): the session is done.
        const std::vector<Snippet::Range> ranges = _s.caretRanges();
        [self end];
        if (!ranges.empty()) [self selectRanges:{ranges.front()}];
        return YES;
    }
    [self selectCurrent];
    return YES;
}

// Copies of the stop being left that a transform makes, written from the
// stop's text as it is now.
- (void)writeTransforms {
    CodeTextView *tv = _tv;
    const std::vector<Snippet::Range> carets = _s.caretRanges();
    if (carets.empty() || !tv) return;
    NSString *s = tv.string;
    const NSRange first = RangeOf(carets.front());
    if (NSMaxRange(first) > s.length) return;
    NSString *value = [s substringWithRange:first];
    std::u16string v(value.length, u'\0');
    [value getCharacters:(unichar *)v.data() range:NSMakeRange(0, value.length)];
    const std::vector<Snippet::Session::Replacement> reps = _s.transformsFor(v);
    if (reps.empty()) return;
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSMutableArray<NSString *> *strings = [NSMutableArray array];
    for (const auto &r : reps) {
        if (r.start + r.length > s.length) return;
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(r.start, r.length)]];
        [strings addObject:[NSString stringWithCharacters:(const unichar *)r.text.data()
                                                   length:r.text.size()]];
    }
    _applying = YES;
    [tv replaceRanges:ranges withStrings:strings actionName:@"Snippet"];
    _applying = NO;
}

- (void)selectRanges:(const std::vector<Snippet::Range> &)ranges {
    CodeTextView *tv = _tv;
    if (!tv || ranges.empty()) return;
    const NSUInteger n = tv.string.length;
    NSMutableArray<NSValue *> *list = [NSMutableArray array];
    for (const Snippet::Range &r : ranges) {
        if (r.end > n) continue;
        [list addObject:[NSValue valueWithRange:RangeOf(r)]];
    }
    if (!list.count) return;
    _applying = YES;
    [tv.multiCarets setCaretRanges:list];
    _applying = NO;
    [tv scrollRangeToVisible:list.firstObject.rangeValue];
}

- (void)selectCurrent {
    const std::vector<Snippet::Range> ranges = _s.caretRanges();
    if (ranges.empty()) {
        [self end];
        return;
    }
    [self invalidate];
    [self selectRanges:ranges];
    const Snippet::Stop &stop = _s.currentStop();
    if (!stop.choices.empty() && _onChoices) {
        NSMutableArray<NSString *> *choices = [NSMutableArray array];
        for (const std::u16string &c : stop.choices)
            [choices addObject:[NSString stringWithCharacters:(const unichar *)c.data()
                                                       length:c.size()]];
        _onChoices(choices, RangeOf(ranges.front()));
    }
}

// ---- following the text

// One replacement, as it is made (MCSourceStorage onReplaceCharacters).
- (void)replaced:(NSRange)range length:(NSUInteger)length {
    if (!_active) return;
    NSUndoManager *um = _tv.undoManager;
    // Undo puts back text the stops knew nothing of.
    if (um.isUndoing || um.isRedoing) {
        [self end];
        return;
    }
    _s.edited(range.location, range.length, length);
    if (!_s.active()) _active = NO;
    [self invalidate];
}

// The same from a storage that only says what an editing session changed
// as a whole.
- (void)storageEdited:(NSNotification *)note {
    if (!_active) return;
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange er = ts.editedRange;
    const NSInteger removed = (NSInteger)er.length - ts.changeInLength;
    if (er.location == NSNotFound || removed < 0) {
        [self end];
        return;
    }
    [self replaced:NSMakeRange(er.location, (NSUInteger)removed) length:er.length];
}

// A selection that leaves the current stop ends the session: looked at on
// the next turn of the run loop, once the carets have settled (several
// carets set the view's selection more than once while they type).
- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    if (!_active || _applying) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(checkSelection)
                                               object:nil];
    [self performSelector:@selector(checkSelection) withObject:nil afterDelay:0];
}

- (void)checkSelection {
    CodeTextView *tv = _tv;
    if (!_active || !tv) return;
    std::vector<Snippet::Range> sel;
    NSArray<NSValue *> *ranges = tv.multiCaretActive ? tv.multiCarets.caretRanges : tv.selectedRanges;
    for (NSValue *v in ranges) sel.push_back({v.rangeValue.location, NSMaxRange(v.rangeValue)});
    if (!_s.holds(sel)) [self end];
}

// ---- drawing

// The boxes are redrawn on the next turn of the run loop: during a storage
// edit the layout manager has not caught up with the text yet, and asking
// it for rects then left it broken (an exception in the next layout).
- (void)invalidate {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(invalidateNow)
                                               object:nil];
    [self performSelector:@selector(invalidateNow) withObject:nil afterDelay:0];
}

- (void)invalidateNow {
    CodeTextView *tv = _tv;
    if (!tv) return;
    if (!NSIsEmptyRect(_drawn)) [tv setNeedsDisplayInRect:_drawn];
    _drawn = NSZeroRect;
    if (!_active || _s.stops().empty()) return;
    const Snippet::Range span = _s.span();
    const NSUInteger n = tv.textStorage.length;
    const NSUInteger a = MIN(span.start, n), b = MIN(span.end, n);
    [tv enumerateRectsForRange:NSMakeRange(a, b - a) block:^(NSRect rect) {
        [tv setNeedsDisplayInRect:NSInsetRect(rect, -4, -2)];
    }];
    if (b == a) {
        // An empty span has no rects of its own: the line around it.
        [tv enumerateRectsForRange:NSMakeRange(a > 0 ? a - 1 : a, MIN(n, a + 1) - (a > 0 ? a - 1 : a))
                             block:^(NSRect rect) {
            [tv setNeedsDisplayInRect:NSInsetRect(rect, -4, -2)];
        }];
    }
}

// An empty stop's place: a thin bar at the caret's x on its line.
- (NSRect)barAt:(NSUInteger)i {
    CodeTextView *tv = _tv;
    const NSUInteger n = tv.textStorage.length;
    __block NSRect bar = NSZeroRect;
    if (i < n) {
        [tv enumerateRectsForRange:NSMakeRange(i, 1) block:^(NSRect r) {
            if (NSIsEmptyRect(bar)) bar = NSMakeRect(NSMinX(r) - 0.5, NSMinY(r), 1.5, NSHeight(r));
        }];
    }
    if (NSIsEmptyRect(bar) && i > 0 && i <= n) {
        [tv enumerateRectsForRange:NSMakeRange(i - 1, 1) block:^(NSRect r) {
            bar = NSMakeRect(NSMaxX(r) - 0.5, NSMinY(r), 1.5, NSHeight(r));
        }];
    }
    return bar;
}

- (void)drawInRect:(NSRect)dirty {
    CodeTextView *tv = _tv;
    if (!_active || !tv) return;
    // The accent (#4EA1F7): the current stop filled faintly and outlined,
    // the others outlined, the final stop a faint bar.
    NSColor *accent = [NSColor colorWithSRGBRed:0x4E / 255.0 green:0xA1 / 255.0
                                           blue:0xF7 / 255.0 alpha:1];
    const NSUInteger n = tv.textStorage.length;
    const std::vector<Snippet::Stop> &stops = _s.stops();
    for (size_t k = 0; k < stops.size(); k++) {
        const Snippet::Stop &stop = stops[k];
        const BOOL current = k == _s.current();
        const BOOL final = stop.index == 0;
        for (size_t r = 0; r < stop.ranges.size(); r++) {
            if (_s.dead(k, r)) continue;
            const Snippet::Range &rg = stop.ranges[r];
            if (rg.end > n) continue;
            if (rg.start == rg.end) {
                const NSRect bar = [self barAt:rg.start];
                if (NSIsEmptyRect(bar)) continue;
                _drawn = NSUnionRect(_drawn, NSInsetRect(bar, -2, -2));
                if (!NSIntersectsRect(bar, dirty)) continue;
                [[accent colorWithAlphaComponent:final ? 0.35 : (current ? 0.8 : 0.5)] set];
                NSRectFillUsingOperation(bar, NSCompositingOperationSourceOver);
                continue;
            }
            [tv enumerateRectsForRange:RangeOf(rg) block:^(NSRect rect) {
                self->_drawn = NSUnionRect(self->_drawn, NSInsetRect(rect, -2, -2));
                if (!NSIntersectsRect(rect, dirty)) return;
                NSRect box = NSInsetRect([tv backingAlignedRect:rect
                                                        options:NSAlignAllEdgesNearest], 0.5, 0.5);
                box.size.height = MAX(4, NSHeight(box) - 1);
                if (current) {
                    [[accent colorWithAlphaComponent:0.14] set];
                    NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
                }
                [[accent colorWithAlphaComponent:current ? 0.55 : 0.3] set];
                NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:box xRadius:2 yRadius:2];
                p.lineWidth = 1;
                [p stroke];
            }];
        }
    }
}

// ---- for tests

- (NSInteger)currentStopIndex {
    return _active ? _s.currentStop().index : -1;
}

- (NSArray<NSValue *> *)currentRanges {
    NSMutableArray *out = [NSMutableArray array];
    if (!_active) return out;
    for (const Snippet::Range &r : _s.caretRanges()) [out addObject:[NSValue valueWithRange:RangeOf(r)]];
    return out;
}

- (NSArray<NSValue *> *)allRanges {
    NSMutableArray *out = [NSMutableArray array];
    if (!_active) return out;
    const std::vector<Snippet::Stop> &stops = _s.stops();
    for (size_t k = 0; k < stops.size(); k++)
        for (size_t r = 0; r < stops[k].ranges.size(); r++)
            if (!_s.dead(k, r)) [out addObject:[NSValue valueWithRange:RangeOf(stops[k].ranges[r])]];
    return out;
}

@end
