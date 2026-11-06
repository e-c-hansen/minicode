// AgentNotes.mm — see AgentNotes.h.
#import "AgentNotes.h"
#import "AppSettings.h"

static NSColor *NHex(unsigned rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

static const unsigned kAccent = 0x4EA1F7;

@implementation MCTourStop
@end

// One note: what it says, who wrote it, where it is, and its place in a tour.
@interface MCNoteState : NSObject
@property(nonatomic, copy) NSString *text, *from, *count;
@property(nonatomic) NSRange range;
@end
@implementation MCNoteState
@end

// ------------------------------------------------------------ the note view
// A box over the text: the writer and the tour count on top, the note
// below, a × to close it and, in a tour, ‹ and › to step. It never takes the
// keyboard, and a click on it goes no further (the caret stays put).
@interface MCNoteView : NSView
@property(nonatomic, strong) NSTextField *label, *body;
@property(nonatomic, strong) NSButton *closeButton, *prevButton, *nextButton;
@property(nonatomic) BOOL showsSteps;
- (CGFloat)heightForWidth:(CGFloat)width;
@end

@implementation MCNoteView

- (NSButton *)button:(NSString *)title {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.bordered = NO;
    b.refusesFirstResponder = YES;
    b.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    b.contentTintColor = NHex(0x9CA3AF, 1);
    [self addSubview:b];
    return b;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.label = [NSTextField labelWithString:@""];
        self.label.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
        self.label.textColor = NHex(kAccent, 1);
        self.label.selectable = NO;
        [self addSubview:self.label];
        self.body = [NSTextField wrappingLabelWithString:@""];
        self.body.font = [NSFont systemFontOfSize:12.5];
        self.body.selectable = NO;
        self.body.refusesFirstResponder = YES;
        [self addSubview:self.body];
        self.closeButton = [self button:@"✕"];
        self.closeButton.toolTip = @"Close the note (Esc)";
        self.prevButton = [self button:@"‹"];
        self.prevButton.toolTip = @"Previous stop (Option+[)";
        self.nextButton = [self button:@"›"];
        self.nextButton.toolTip = @"Next stop (Option+])";
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return NO; }
- (void)mouseDown:(NSEvent *)event { (void)event; }
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:NSCursor.arrowCursor]; }

static const CGFloat kPadX = 12, kPadTop = 6, kHeaderH = 16, kGap = 3, kPadBottom = 8;

- (CGFloat)bodyWidthFor:(CGFloat)width { return MAX(40, width - kPadX - 10); }

- (CGFloat)heightForWidth:(CGFloat)width {
    NSSize s = [self.body.cell cellSizeForBounds:
        NSMakeRect(0, 0, [self bodyWidthFor:width], CGFLOAT_MAX)];
    return ceil(kPadTop + kHeaderH + kGap + s.height + kPadBottom);
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    const CGFloat W = size.width;
    CGFloat right = W - 6;
    self.closeButton.frame = NSMakeRect(right - 20, kPadTop - 2, 20, 18);
    right -= 22;
    self.nextButton.hidden = self.prevButton.hidden = !self.showsSteps;
    if (self.showsSteps) {
        self.nextButton.frame = NSMakeRect(right - 18, kPadTop - 2, 18, 18);
        self.prevButton.frame = NSMakeRect(right - 38, kPadTop - 2, 18, 18);
        right -= 40;
    }
    self.label.frame = NSMakeRect(kPadX, kPadTop, MAX(0, right - kPadX), kHeaderH);
    self.body.frame = NSMakeRect(kPadX, kPadTop + kHeaderH + kGap, [self bodyWidthFor:W],
                                 MAX(0, size.height - kPadTop - kHeaderH - kGap - kPadBottom));
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    AppSettings *cfg = [AppSettings shared];
    NSColor *editor = [[cfg background:Surface::Editor] colorWithAlphaComponent:1];
    NSColor *fill = [editor blendedColorWithFraction:0.08 ofColor:NHex(kAccent, 1)] ?: editor;
    NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *box = [NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5];
    [fill set];
    [box fill];
    [NHex(kAccent, 0.55) set];
    box.lineWidth = 1;
    [box stroke];
    [NSGraphicsContext saveGraphicsState];
    [box addClip];
    [NHex(kAccent, 1) set];
    NSRectFill(NSMakeRect(0, 0, 3, NSHeight(self.bounds)));
    [NSGraphicsContext restoreGraphicsState];
}
@end

// ------------------------------------------------------------- the notes
@implementation MCAgentNotes {
    __weak id<MCAgentNotesHost> _host;
    NSMapTable<id, MCNoteState *> *_notes;   // by tab, weakly
    MCNoteView *_view;
    __weak NSTextView *_observed;
    MCNoteState *_shown;                     // on screen now
    BOOL _suspended;                         // a tab change is swapping the text
    NSArray<MCTourStop *> *_tour;
    NSInteger _tourIndex;
    NSString *_tourFrom;
}

- (instancetype)initWithHost:(id<MCAgentNotesHost>)host {
    if ((self = [super init])) {
        _host = host;
        _notes = [NSMapTable weakToStrongObjectsMapTable];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// ---------------------------------------------------------------- geometry
static NSUInteger MCLineStart(NSString *s, int line) {
    NSUInteger index = 0;
    for (int l = 1; l < line && index < s.length; l++) {
        NSRange nl = [s rangeOfString:@"\n" options:NSLiteralSearch
                                range:NSMakeRange(index, s.length - index)];
        if (nl.location == NSNotFound) return s.length;
        index = NSMaxRange(nl);
    }
    return index;
}

static NSUInteger MCLineEnd(NSString *s, NSUInteger start) {
    NSRange nl = [s rangeOfString:@"\n" options:NSLiteralSearch
                            range:NSMakeRange(start, s.length - start)];
    NSUInteger end = nl.location == NSNotFound ? s.length : nl.location;
    if (end > start && [s characterAtIndex:end - 1] == '\r') end--;
    return end;
}

// A 1-based column within the line from `start`, in characters.
static NSUInteger MCColumn(NSString *s, NSUInteger start, NSUInteger end, int column) {
    NSUInteger i = start;
    for (int c = 1; c < column && i < end; c++)
        i = NSMaxRange([s rangeOfComposedCharacterSequenceAtIndex:i]);
    return MIN(i, end);
}

static NSRange MCNoteRange(NSString *s, int line, int column, int toLine, int toColumn) {
    if (line < 1) line = 1;
    if (toLine < line) { toLine = line; if (toColumn && !column) toColumn = 0; }
    NSUInteger ls = MCLineStart(s, line);
    NSUInteger le = MCLineEnd(s, ls);
    NSUInteger start = column > 0 ? MCColumn(s, ls, le, column) : ls;
    NSUInteger ts = toLine == line ? ls : MCLineStart(s, toLine);
    NSUInteger te = MCLineEnd(s, ts);
    NSUInteger end = toColumn > 0 ? MCColumn(s, ts, te, toColumn) : te;
    if (end < start) end = start;
    return NSMakeRange(start, end - start);
}

// The rect of the line holding character `index`, in the text view.
- (NSRect)lineRectAt:(NSUInteger)index in:(NSTextView *)tv {
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger n = tv.string.length;
    NSRect r;
    if (n == 0 || index >= n) {
        [lm ensureLayoutForTextContainer:tv.textContainer];
        r = lm.extraLineFragmentRect;
        if (NSIsEmptyRect(r) && n > 0)
            r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:n - 1]
                                     effectiveRange:nil];
    } else {
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, index + 1)];
        r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:index]
                                 effectiveRange:nil];
    }
    NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

// ------------------------------------------------------------ observation
- (void)observe:(NSTextView *)tv {
    if (_observed == tv) return;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (_observed) {
        [nc removeObserver:self name:NSTextStorageDidProcessEditingNotification
                    object:_observed.textStorage];
        [nc removeObserver:self name:NSViewFrameDidChangeNotification object:_observed];
    }
    _observed = tv;
    [nc addObserver:self selector:@selector(storageEdited:)
               name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
    tv.postsFrameChangedNotifications = YES;
    [nc addObserver:self selector:@selector(textFrameChanged:)
               name:NSViewFrameDidChangeNotification object:tv];
}

// An edit that touches the note's text closes it for good; one before it
// moves it along. A tab change swaps the whole text, which is no edit.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (_suspended || !_shown || !(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange edited = ts.editedRange;
    const NSInteger delta = ts.changeInLength;
    const NSUInteger oldLength = (NSInteger)edited.length - delta > 0 ? edited.length - delta : 0;
    const NSUInteger s = _shown.range.location, e = NSMaxRange(_shown.range);
    if (edited.location + oldLength < s) {
        NSRange r = _shown.range;
        r.location = (NSUInteger)MAX((NSInteger)0, (NSInteger)r.location + delta);
        _shown.range = r;
        dispatch_async(dispatch_get_main_queue(), ^{ [self placeView]; });
    } else if (edited.location <= e) {
        // The highlight goes once the edit is done, from the text as it is
        // then: the layout manager is in the middle of it now.
        NSRange old = _shown.range;
        [self forgetShownKeepingHighlight];
        _tour = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self removeHighlight:NSUnionRange(old, edited)];
        });
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ [self placeView]; });
    }
}

- (void)textFrameChanged:(NSNotification *)note {
    (void)note;
    if (_shown && !_suspended) [self placeView];
}

// ---------------------------------------------------------------- display
// The highlight runs on to the last line's break, so every line of it is a
// band across the pane, the last one too.
static NSRange MCBand(NSString *s, NSRange range) {
    const NSUInteger n = s.length;
    if (range.location > n) return NSMakeRange(n, 0);
    range.length = MIN(range.length, n - range.location);
    if (NSMaxRange(range) < n && [s characterAtIndex:NSMaxRange(range)] == '\r') range.length++;
    if (NSMaxRange(range) < n && [s characterAtIndex:NSMaxRange(range)] == '\n') range.length++;
    return range;
}

- (void)removeHighlight:(NSRange)range {
    NSTextView *tv = _observed;
    const NSUInteger n = tv.string.length;
    if (!tv || n == 0 || range.location >= n) return;
    range = MCBand(tv.string, range);
    [tv.layoutManager removeTemporaryAttribute:NSBackgroundColorAttributeName
                             forCharacterRange:range];
}

- (void)addHighlight:(NSRange)range {
    NSTextView *tv = _observed;
    const NSUInteger n = tv.string.length;
    if (!tv || range.location > n) return;
    range = MCBand(tv.string, range);
    if (range.length == 0) return;
    [tv.layoutManager addTemporaryAttribute:NSBackgroundColorAttributeName
                                      value:NHex(kAccent, 0.18)
                          forCharacterRange:range];
}

// The note view leaves the screen; the note stays with its tab.
- (void)hideShown {
    if (!_shown) return;
    [self removeHighlight:_shown.range];
    [_view removeFromSuperview];
    _shown = nil;
}

// After an edit inside the note: it is gone, from its tab too.
- (void)forgetShownKeepingHighlight {
    id tab = [_host notesActiveTab];
    if (tab && [_notes objectForKey:tab] == _shown) [_notes removeObjectForKey:tab];
    [_view removeFromSuperview];
    _shown = nil;
}

- (MCNoteView *)view {
    if (!_view) {
        _view = [[MCNoteView alloc] initWithFrame:NSMakeRect(0, 0, 300, 60)];
        _view.closeButton.target = self;
        _view.closeButton.action = @selector(closeNote);
        _view.prevButton.target = self;
        _view.prevButton.action = @selector(previousStop:);
        _view.nextButton.target = self;
        _view.nextButton.action = @selector(nextStop:);
    }
    return _view;
}

- (void)previousStop:(id)sender { (void)sender; if (![self stepTour:-1]) NSBeep(); }
- (void)nextStop:(id)sender { (void)sender; if (![self stepTour:1]) NSBeep(); }

- (void)display:(MCNoteState *)note scroll:(BOOL)scroll {
    NSTextView *tv = [_host notesTextView];
    if (!tv) return;
    [self observe:tv];
    const NSUInteger n = tv.string.length;
    NSRange r = note.range;
    if (r.location > n) r = NSMakeRange(n, 0);
    if (NSMaxRange(r) > n) r.length = n - r.location;
    note.range = r;
    _shown = note;
    [self addHighlight:r];
    if (note.text.length) {
        MCNoteView *v = [self view];
        AppSettings *cfg = [AppSettings shared];
        v.body.textColor = [cfg text:Surface::Editor];
        v.body.stringValue = note.text;
        NSString *who = note.from.length ? note.from : @"Claude";
        v.label.stringValue = note.count ? [NSString stringWithFormat:@"%@  ·  %@", who, note.count]
                                         : who;
        v.showsSteps = note.count != nil;
        if (v.superview != tv) [tv addSubview:v];
    } else {
        [_view removeFromSuperview];
    }
    [self placeView];
    if (scroll) [self scrollToShown];
}

// The note over the first line of its range, or under the last when the
// range starts at the top of the text.
- (void)placeView {
    NSTextView *tv = _observed;
    if (!_shown || !tv || !_view.superview) return;
    const NSRect first = [self lineRectAt:_shown.range.location in:tv];
    const CGFloat x = tv.textContainerOrigin.x + tv.textContainer.lineFragmentPadding + 24;
    const CGFloat width = MAX(220, MIN(640, NSWidth(tv.bounds) - x - 24));
    const CGFloat h = [_view heightForWidth:width];
    CGFloat y = NSMinY(first) - h - 3;
    if (y < 0) {
        const NSUInteger last = NSMaxRange(_shown.range) > _shown.range.location
                                    ? NSMaxRange(_shown.range) - 1 : _shown.range.location;
        y = NSMaxY([self lineRectAt:last in:tv]) + 3;
    }
    _view.frame = NSMakeRect(x, y, width, h);
    _view.needsDisplay = YES;
}

// Scrolls so that `lineTop` (in the text view) is `above` points below the
// top of the pane, as near as the text's length allows.
static void MCScrollLine(NSTextView *tv, CGFloat lineTop, CGFloat above) {
    NSScrollView *scroll = tv.enclosingScrollView;
    if (!scroll) return;
    NSClipView *clip = scroll.contentView;
    const CGFloat H = NSHeight(clip.bounds);
    // Laid out a screen past it too, or the text view is still only as tall
    // as the layout so far and the scroll stops short (the line ends up at
    // the bottom of the pane).
    [tv.layoutManager ensureLayoutForBoundingRect:NSMakeRect(0, 0, CGFLOAT_MAX, lineTop + 2 * H)
                                  inTextContainer:tv.textContainer];
    CGFloat y = MIN(lineTop - above, NSHeight(tv.frame) - H);
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, MAX(0, y))];
    [scroll reflectScrolledClipView:clip];
}

// The range's first line a third of the way down the pane, with room for
// the note above it.
- (void)scrollToShown {
    NSTextView *tv = _observed;
    NSScrollView *scroll = tv.enclosingScrollView;
    if (!_shown || !scroll) return;
    const NSRect first = [self lineRectAt:_shown.range.location in:tv];
    const CGFloat H = NSHeight(scroll.contentView.bounds);
    const CGFloat noteH = _view.superview ? NSHeight(_view.frame) + 12 : 0;
    MCScrollLine(tv, NSMinY(first), MAX(round(H / 3), noteH));
}

// ------------------------------------------------------------------- API
- (BOOL)showNote:(NSString *)text from:(NSString *)from
            line:(int)line column:(int)column toLine:(int)toLine toColumn:(int)toColumn {
    _tour = nil;   // a note of its own replaces a tour
    return [self putNote:text from:from count:nil line:line column:column
                  toLine:toLine toColumn:toColumn];
}

- (BOOL)putNote:(NSString *)text from:(NSString *)from count:(NSString *)count
           line:(int)line column:(int)column toLine:(int)toLine toColumn:(int)toColumn {
    NSTextView *tv = [_host notesTextView];
    id tab = [_host notesActiveTab];
    if (!tv || !tab) return NO;
    [self observe:tv];
    [self hideShown];
    MCNoteState *note = [MCNoteState new];
    note.text = text ?: @"";
    note.from = from;
    note.count = count;
    note.range = MCNoteRange(tv.string, line, column, toLine, toColumn);
    [_notes setObject:note forKey:tab];
    tv.selectedRange = NSMakeRange(note.range.location, 0);
    [self display:note scroll:YES];
    return YES;
}

- (BOOL)revealLine:(int)line column:(int)column {
    NSTextView *tv = [_host notesTextView];
    if (!tv) return NO;
    NSString *s = tv.string;
    NSUInteger ls = MCLineStart(s, MAX(line, 1));
    NSUInteger le = MCLineEnd(s, ls);
    NSUInteger at = column > 0 ? MCColumn(s, ls, le, column) : ls;
    tv.selectedRange = NSMakeRange(at, 0);
    if (NSScrollView *scroll = tv.enclosingScrollView)
        MCScrollLine(tv, NSMinY([self lineRectAt:at in:tv]),
                     round(NSHeight(scroll.contentView.bounds) / 3));
    if (le > ls && tv.window) [tv showFindIndicatorForRange:NSMakeRange(ls, le - ls)];
    return YES;
}

- (void)closeNote {
    id tab = [_host notesActiveTab];
    if (tab) [_notes removeObjectForKey:tab];
    [self hideShown];
    _tour = nil;
}

- (BOOL)noteShown { return _shown != nil; }
- (NSString *)noteText { return _shown.text; }
- (NSString *)noteLabel {
    if (!_shown) return nil;
    NSString *who = _shown.from.length ? _shown.from : @"Claude";
    return _shown.count ? [NSString stringWithFormat:@"%@ · %@", who, _shown.count] : who;
}
- (NSRange)noteRange { return _shown ? _shown.range : NSMakeRange(NSNotFound, 0); }
- (NSView *)noteView { return _shown && _view.superview ? _view : nil; }

// ------------------------------------------------------------------ tours
- (BOOL)tourActive { return _tour != nil; }
- (NSInteger)tourIndex { return _tour ? _tourIndex : -1; }
- (NSInteger)tourCount { return (NSInteger)_tour.count; }

- (BOOL)startTour:(NSArray<MCTourStop *> *)stops from:(NSString *)from {
    if (!stops.count) return NO;
    NSArray<MCTourStop *> *tour = [stops copy];
    _tour = tour;
    _tourFrom = [from copy];
    _tourIndex = 0;
    if ([self goToStop:0 of:tour]) return YES;
    if (_tour == tour) _tour = nil;
    return NO;
}

- (BOOL)goToStop:(NSInteger)i of:(NSArray<MCTourStop *> *)tour {
    MCTourStop *stop = tour[(NSUInteger)i];
    // Opening the stop swaps tabs, which keeps the tour (it is no edit).
    if (![_host openFileForAgent:stop.path preview:YES]) return NO;
    _tour = tour;
    _tourIndex = i;
    NSString *count = [NSString stringWithFormat:@"%ld of %lu", (long)i + 1,
                       (unsigned long)tour.count];
    return [self putNote:stop.note from:_tourFrom count:count line:stop.line
                  column:stop.column toLine:stop.toLine toColumn:0];
}

- (BOOL)stepTour:(NSInteger)delta {
    if (!_tour) return NO;
    NSInteger next = _tourIndex + delta;
    if (next < 0 || next >= (NSInteger)_tour.count) return NO;
    return [self goToStop:next of:_tour];
}

// ---------------------------------------------------------------- hooks
- (void)activeTabWillChange {
    _suspended = YES;
    [self hideShown];
}

- (void)activeTabDidChange {
    _suspended = NO;
    id tab = [_host notesActiveTab];
    MCNoteState *note = tab ? [_notes objectForKey:tab] : nil;
    if (note && [_host notesTextView]) [self display:note scroll:NO];
}

- (BOOL)handleCommand:(SEL)selector {
    if (!_shown) return NO;
    if (selector == @selector(cancelOperation:) || selector == @selector(complete:)) {
        [self closeNote];
        return YES;
    }
    return NO;
}
@end
