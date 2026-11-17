// SpellMarks.mm — Objective-C++. See SpellMarks.h.
#import "SpellMarks.h"
#import "Lsp.h"        // CodeTextView
#import "FoldView.h"   // MCFolds rangeIsHidden:
#include "SpellCheck.h"

namespace {
// How long typing must have stopped before the lines on screen are checked.
const NSTimeInterval kPauseAfterTyping = 0.5;

dispatch_queue_t SpellQueue() {
    static dispatch_queue_t q = dispatch_queue_create("minicode.spell", DISPATCH_QUEUE_SERIAL);
    return q;
}
}  // namespace

@implementation MCSpellMarks {
    __weak CodeTextView *_tv;
    NSMutableArray<NSValue *> *_marks;
    NSMutableIndexSet *_checked;   // characters checked since the last edit
    NSUInteger _generation;        // counts edits and documents
    NSString *_key;
    Spell::Kind _kind;
    NSColor *_color;
    MCSpellLinesProvider _lines;
    NSMutableDictionary<NSString *, NSNumber *> *_tags;   // a spell document tag per key
    CFTimeInterval _lastEdit;
    BOOL _scheduled;
    __weak NSClipView *_observedClip;
}

- (instancetype)initWithTextView:(CodeTextView *)textView {
    if ((self = [super init])) {
        _tv = textView;
        _marks = [NSMutableArray array];
        _checked = [NSMutableIndexSet indexSet];
        _tags = [NSMutableDictionary dictionary];
        _kind = Spell::Kind::None;
        _color = [NSColor systemBlueColor];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (NSArray<NSValue *> *)marks { return [_marks copy]; }

- (NSInteger)tag {
    NSString *key = _key ?: @"";
    NSNumber *t = _tags[key];
    if (!t) {
        t = @([NSSpellChecker uniqueSpellDocumentTag]);
        _tags[key] = t;
    }
    return t.integerValue;
}

- (void)showDocument:(NSString *)key
                kind:(NSInteger)kind
               color:(NSColor *)color
               lines:(MCSpellLinesProvider)lines {
    _key = [key copy];
    _kind = (Spell::Kind)kind;
    _color = color ?: [NSColor systemBlueColor];
    _lines = [lines copy];
    _generation++;
    [_checked removeAllIndexes];
    [self clearMarks];
    if (_kind == Spell::Kind::None) return;
    [self observeScrolling];
    [self scheduleAfter:0.05];
}

- (void)observeScrolling {
    NSClipView *clip = _tv.enclosingScrollView.contentView;
    if (!clip || clip == _observedClip) return;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (_observedClip)
        [nc removeObserver:self name:NSViewBoundsDidChangeNotification object:_observedClip];
    clip.postsBoundsChangedNotifications = YES;
    [nc addObserver:self
           selector:@selector(viewScrolled:)
               name:NSViewBoundsDidChangeNotification
             object:clip];
    _observedClip = clip;
}

- (void)viewScrolled:(NSNotification *)note {
    (void)note;
    if (_kind != Spell::Kind::None) [self scheduleAfter:0.15];
}

- (void)clearMarks {
    if (!_marks.count) return;
    [_marks removeAllObjects];
    [_tv setNeedsDisplay:YES];
}

// ---- following edits

// A mark after the edit moves with its text; one the edit touches (a
// letter typed at its end too) goes, until the next check.
- (void)textEdited:(NSTextStorage *)ts {
    _generation++;
    [_checked removeAllIndexes];
    if (_marks.count) {
        const NSRange er = ts.editedRange;
        const NSInteger delta = ts.changeInLength;
        const NSUInteger s = er.location;
        const NSUInteger oldEnd = (NSUInteger)((NSInteger)NSMaxRange(er) - delta);
        BOOL dropped = NO;
        NSMutableArray<NSValue *> *kept = [NSMutableArray arrayWithCapacity:_marks.count];
        for (NSValue *v in _marks) {
            NSRange r = v.rangeValue;
            if (NSMaxRange(r) < s) {
                [kept addObject:v];
            } else if (r.location > oldEnd) {
                r.location = (NSUInteger)((NSInteger)r.location + delta);
                [kept addObject:[NSValue valueWithRange:r]];
            } else {
                dropped = YES;
            }
        }
        _marks = kept;
        if (dropped) [_tv setNeedsDisplay:YES];
    }
    if (_kind == Spell::Kind::None) return;
    _lastEdit = CACurrentMediaTime();
    [self scheduleAfter:kPauseAfterTyping];
}

// One pending check at a time, however many keys or scroll events: when it
// fires before typing has paused long enough, it waits out the rest.
- (void)scheduleAfter:(NSTimeInterval)delay {
    if (_scheduled) return;
    _scheduled = YES;
    [self performSelector:@selector(fire)
               withObject:nil
               afterDelay:delay
                  inModes:@[NSRunLoopCommonModes]];
}

- (void)fire {
    _scheduled = NO;
    const CFTimeInterval wait = _lastEdit + kPauseAfterTyping - CACurrentMediaTime();
    if (wait > 0.01) {
        [self scheduleAfter:wait];
        return;
    }
    [self checkVisible];
}

- (void)checkNow {
    _lastEdit = 0;
    [self checkVisible];
}

// ---- checking

- (void)checkVisible {
    CodeTextView *tv = _tv;
    if (!tv) return;
    if (_kind == Spell::Kind::None || !tv.editable || !_lines) {
        [self clearMarks];
        return;
    }
    [self observeScrolling];
    NSTextStorage *ts = tv.textStorage;
    const NSUInteger total = ts.length;
    if (!total) return;
    NSLayoutManager *lm = tv.layoutManager;
    const NSRange glyphs = [lm glyphRangeForBoundingRect:tv.visibleRect
                                         inTextContainer:tv.textContainer];
    NSRange visible = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    if (visible.location >= total || !visible.length) return;
    visible.length = MIN(visible.length, total - visible.location);
    if ([_checked containsIndexesInRange:visible]) return;

    std::vector<Token> tokens;
    const NSRange lines = _lines(visible, &tokens);
    if (!lines.length || NSMaxRange(lines) > total) return;
    // Markdown reads from the top (a fence opened above decides what
    // follows); the others need only their lines, the tokens made relative.
    const Spell::Kind kind = _kind;
    const BOOL fromTop = kind == Spell::Kind::Markdown;
    NSString *text = fromTop ? [ts.string substringToIndex:NSMaxRange(lines)]
                             : [ts.string substringWithRange:lines];
    const size_t from = fromTop ? lines.location : 0;
    if (!fromTop) {
        std::vector<Token> relative;
        relative.reserve(tokens.size());
        for (const Token &t : tokens) {
            if (t.start + t.length <= lines.location || t.start >= NSMaxRange(lines)) continue;
            const size_t s = MAX(t.start, lines.location);
            const size_t e = MIN(t.start + t.length, NSMaxRange(lines));
            relative.push_back({s - lines.location, e - s, t.style});
        }
        tokens.swap(relative);
    }
    const NSUInteger generation = _generation;
    const NSInteger tag = [self tag];
    __weak MCSpellMarks *weakSelf = self;
    const std::vector<Token> blockTokens = std::move(tokens);
    dispatch_async(SpellQueue(), ^{
        std::u16string u(text.length, u' ');
        [text getCharacters:(unichar *)u.data() range:NSMakeRange(0, text.length)];
        const std::vector<Spell::Span> words = Spell::checkable(kind, u, from, blockTokens);
        // Spaces as long as the lines, with the words in their places.
        std::u16string masked(u.size() - from, u' ');
        for (const Spell::Span &w : words)
            for (size_t i = w.first; i < w.second; i++) masked[i - from] = u[i];
        NSString *m = words.empty() ? nil
                                    : [NSString stringWithCharacters:(const unichar *)masked.data()
                                                              length:masked.size()];
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf check:m lines:lines tag:tag generation:generation];
        });
    });
}

- (void)check:(NSString *)masked
        lines:(NSRange)lines
          tag:(NSInteger)tag
   generation:(NSUInteger)generation {
    if (generation != _generation) return;
    if (!masked) {
        [self take:@[] lines:lines generation:generation];
        return;
    }
    __weak MCSpellMarks *weakSelf = self;
    const NSUInteger origin = lines.location;
    [[NSSpellChecker sharedSpellChecker]
        requestCheckingOfString:masked
                          range:NSMakeRange(0, masked.length)
                          types:NSTextCheckingTypeSpelling
                        options:nil
         inSpellDocumentWithTag:tag
              completionHandler:^(NSInteger sequence, NSArray<NSTextCheckingResult *> *results,
                                  NSOrthography *orthography, NSInteger wordCount) {
        (void)sequence; (void)orthography; (void)wordCount;
        NSMutableArray<NSValue *> *found = [NSMutableArray array];
        for (NSTextCheckingResult *r in results)
            if (r.resultType == NSTextCheckingTypeSpelling && r.range.length)
                [found addObject:[NSValue valueWithRange:NSMakeRange(origin + r.range.location,
                                                                     r.range.length)]];
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf take:found lines:lines generation:generation];
        });
    }];
}

// The checker's answer for `lines`: their marks replaced.
- (void)take:(NSArray<NSValue *> *)found lines:(NSRange)lines generation:(NSUInteger)generation {
    if (generation != _generation) return;
    NSMutableArray<NSValue *> *marks = [NSMutableArray arrayWithCapacity:_marks.count + found.count];
    for (NSValue *v in _marks)
        if (!NSIntersectionRange(v.rangeValue, lines).length) [marks addObject:v];
    [marks addObjectsFromArray:found];
    [marks sortUsingComparator:^NSComparisonResult(NSValue *a, NSValue *b) {
        const NSUInteger x = a.rangeValue.location, y = b.rangeValue.location;
        return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
    }];
    const BOOL changed = ![marks isEqualToArray:_marks];
    _marks = marks;
    [_checked addIndexesInRange:lines];
    _checksDone++;
    if (changed) [_tv setNeedsDisplay:YES];
    // Scrolled on while this was out: the lines now on screen.
    [self scheduleAfter:0.05];
}

// ---- drawing

// Dots along the bottom of each word's line fragments, as the hint
// diagnostics' are drawn (CodeTextView), in editor.spell-check-color.
- (void)drawInRect:(NSRect)dirty {
    CodeTextView *tv = _tv;
    if (!_marks.count || !tv.editable) return;
    const NSUInteger total = tv.textStorage.length;
    [_color set];
    for (NSValue *v in _marks) {
        const NSRange r = v.rangeValue;
        if (NSMaxRange(r) > total) continue;
        if ([tv.folds rangeIsHidden:r]) continue;
        [tv enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(NSInsetRect(rect, 0, -3), dirty) || rect.size.width <= 0) return;
            const CGFloat y = NSMaxY(rect) - 2;
            for (CGFloat x = rect.origin.x + 0.5; x < NSMaxX(rect) - 0.5; x += 3)
                NSRectFillUsingOperation(NSMakeRect(x, y, 1.5, 1.5),
                                         NSCompositingOperationSourceOver);
        }];
    }
}

// ---- the menu

- (NSValue *)markAt:(NSUInteger)index {
    for (NSValue *v in _marks) {
        const NSRange r = v.rangeValue;
        if (r.location <= index && index <= NSMaxRange(r)) return v;
        if (r.location > index) break;
    }
    return nil;
}

- (NSArray<NSMenuItem *> *)itemsAtIndex:(NSUInteger)index {
    CodeTextView *tv = _tv;
    NSValue *mark = tv.editable ? [self markAt:index] : nil;
    if (!mark || NSMaxRange(mark.rangeValue) > tv.textStorage.length) return @[];
    NSString *word = [tv.string substringWithRange:mark.rangeValue];
    NSArray<NSString *> *guesses =
        [[NSSpellChecker sharedSpellChecker] guessesForWordRange:NSMakeRange(0, word.length)
                                                        inString:word
                                                        language:nil
                                          inSpellDocumentWithTag:[self tag]];
    NSMutableArray<NSMenuItem *> *items = [NSMutableArray array];
    for (NSString *g in guesses) {
        if (items.count == 6) break;
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:g
                                                    action:@selector(correctSpelling:)
                                             keyEquivalent:@""];
        it.target = self;
        it.representedObject = @{@"range": mark, @"word": word, @"guess": g};
        [items addObject:it];
    }
    if (!items.count) {
        NSMenuItem *none = [[NSMenuItem alloc] initWithTitle:@"No Guesses Found"
                                                      action:nil
                                               keyEquivalent:@""];
        none.enabled = NO;
        [items addObject:none];
    }
    [items addObject:[NSMenuItem separatorItem]];
    NSMenuItem *learn = [[NSMenuItem alloc] initWithTitle:@"Learn Spelling"
                                                   action:@selector(learnSpelling:)
                                            keyEquivalent:@""];
    NSMenuItem *ignore = [[NSMenuItem alloc] initWithTitle:@"Ignore Spelling"
                                                    action:@selector(ignoreSpelling:)
                                             keyEquivalent:@""];
    for (NSMenuItem *it in @[learn, ignore]) {
        it.target = self;
        it.representedObject = @{@"range": mark, @"word": word};
        [items addObject:it];
    }
    return items;
}

- (void)addItemsToMenu:(NSMenu *)menu atIndex:(NSUInteger)index {
    NSArray<NSMenuItem *> *items = [self itemsAtIndex:index];
    if (!items.count || !menu) return;
    NSInteger at = 0;
    for (NSMenuItem *it in items) [menu insertItem:it atIndex:at++];
    if (menu.numberOfItems > at) [menu insertItem:[NSMenuItem separatorItem] atIndex:at];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    return _tv.editable;
}

- (void)correctSpelling:(NSMenuItem *)sender {
    NSDictionary *d = sender.representedObject;
    CodeTextView *tv = _tv;
    const NSRange r = [d[@"range"] rangeValue];
    if (!tv.editable || NSMaxRange(r) > tv.textStorage.length ||
        ![[tv.string substringWithRange:r] isEqualToString:d[@"word"]])
        return;
    [tv replaceRanges:@[[NSValue valueWithRange:r]]
          withStrings:@[d[@"guess"]]
           actionName:@"Correct Spelling"];
}

- (void)learnSpelling:(NSMenuItem *)sender {
    NSString *word = [sender.representedObject objectForKey:@"word"];
    [[NSSpellChecker sharedSpellChecker] learnWord:word];
    [self forgetWord:word];
}

- (void)ignoreSpelling:(NSMenuItem *)sender {
    NSString *word = [sender.representedObject objectForKey:@"word"];
    [[NSSpellChecker sharedSpellChecker] ignoreWord:word inSpellDocumentWithTag:[self tag]];
    [self forgetWord:word];
}

// Every mark on `word` goes at once; the checker leaves it alone from now on.
- (void)forgetWord:(NSString *)word {
    CodeTextView *tv = _tv;
    NSString *text = tv.string;
    NSMutableArray<NSValue *> *kept = [NSMutableArray array];
    for (NSValue *v in _marks) {
        const NSRange r = v.rangeValue;
        if (NSMaxRange(r) <= text.length && [[text substringWithRange:r] isEqualToString:word])
            continue;
        [kept addObject:v];
    }
    _marks = kept;
    [tv setNeedsDisplay:YES];
}

@end
